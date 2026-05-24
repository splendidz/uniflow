// ======================================================================
//  uniflow.hpp - single-threaded, step-driven cooperative async framework
//
//  A module expresses its logic as a chain of step functions. One pump thread
//  per runtime drives every module round-robin; blocking work is offloaded to
//  a thread pool and its result is delivered to the next step. The runtime is
//  hidden - you never hold it. See DESIGN.md for the rationale.
//
//  Typical usage:
//
//      class OrderRouter : public uniflow::Uniflow<OrderRouter> {
//          UF_SINGLETON(OrderRouter);                  // one instance, inst()
//      public:
//          StepResult OnRoute_Begin(Message m) {       // entry step: public
//              msg_ = std::move(m);
//              return UF_NEXT(OnRoute_Dispatch);
//          }
//      private:
//          StepResult OnRoute_Dispatch() {             // internal step: private
//              UF_ASYNC(DoLookup, msg_.key);           // static fn, no `this`
//              return UF_NEXT(OnRoute_LookupDone);
//          }
//          StepResult OnRoute_LookupDone() {
//              auto r = AsyncResult<Record>();
//              if (r.is_timeout() || r.failed()) return Fail();
//              return Done();
//          }
//          static Record DoLookup(std::string key);    // runs on the pool
//          Message msg_;
//      };
//
//      int main() {
//          OrderRouter::inst().StartFlow(&OrderRouter::OnRoute_Begin, Message{...});
//          OrderRouter::inst().WaitUntilIdle();
//      }
//
//  Single header, C++17.
// ======================================================================
#pragma once

#include <algorithm>
#include <any>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>


namespace uniflow
{

// ----------------------------------------------------------------------
//  Time
// ----------------------------------------------------------------------
using Clock     = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration  = Clock::duration;

inline double to_ms(Duration d)
{
    return std::chrono::duration<double, std::milli>(d).count();
}

// ----------------------------------------------------------------------
//  Step result vocabulary - a step returns an *intent*, not a state change.
// ----------------------------------------------------------------------
enum class StepAction : uint8_t
{
    Stay,    // re-run the same step next tick (busy cooperative poll)
    Wait,    // re-run the same step, but only after a delay (paced poll)
    Advance, // invoke result.next_fn next tick
    Done,    // flow completed normally -> idle
    Fail     // flow aborted -> idle
};

inline const char* to_string(StepAction a)
{
    switch (a)
    {
    case StepAction::Stay:    return "Stay";
    case StepAction::Wait:    return "Wait";
    case StepAction::Advance: return "Advance";
    case StepAction::Done:    return "Done";
    case StepAction::Fail:    return "Fail";
    }
    return "?";
}

// ----------------------------------------------------------------------
//  Async support types
// ----------------------------------------------------------------------

// Per-submission knobs for one UF_ASYNC call. Defaults: no timeout, no slow
// warning, run on the pool named "default".
struct AsyncOpts
{
    Duration                timeout = Duration::max(); // give-up deadline
    std::optional<Duration> slow_warn_after;           // warn-if-still-pending
    std::string             executor_name = "default"; // which pool to use
};

// Stored in the async result slot when a submission exceeds its timeout.
struct AsyncTimeout : std::runtime_error
{
    AsyncTimeout(const char* job, Duration elapsed)
        : std::runtime_error(std::string("async timeout: ") + job),
          job_label(job), elapsed(elapsed) {}
    const char* job_label;
    Duration    elapsed;
};

// A non-owning view of the most-recent async result, handed to the
// continuation step by AsyncResult<T>(). Exactly one of three states:
//   - failed()     : the worker threw            -> exception() holds it
//   - is_timeout() : the worker missed its deadline
//   - neither      : success                     -> value() is valid
// `_val` points into the owning module's slot - use it before the step
// returns; do not stash the AsyncRef itself.
template <class T>
struct AsyncRef
{
    T*                 _val;     // -> module's slot (null on fail/timeout)
    std::exception_ptr _exc;     // set if the worker threw
    bool               _timeout; // set if the deadline was missed

    bool                      failed() const { return _exc != nullptr; }
    bool                      is_timeout() const { return _timeout; }
    T&                        value() const { return *_val; } // valid iff !failed && !timeout
    const std::exception_ptr& exception() const { return _exc; }
};

// ----------------------------------------------------------------------
//  Executor abstraction (whatever thread pool you bring)
// ----------------------------------------------------------------------
class IExecutor
{
public:
    virtual ~IExecutor()                              = default;
    virtual void   Submit(std::function<void()> task) = 0;
    virtual size_t QueueDepth() const                 = 0;
};

// Runs the task synchronously on the calling thread. Useful for tests - the
// whole flow becomes deterministic. NOT for production: it blocks the pump
// thread inside UF_ASYNC and trips the SLOW CPU step warning on every submit.
class InlineExecutor : public IExecutor
{
public:
    void   Submit(std::function<void()> task) override { task(); }
    size_t QueueDepth() const override { return 0; }
};

// Minimal fixed-size worker pool on std::thread + condvar queue. Bundled so
// the framework runs out of the box; swap in your own IExecutor in production.
class StdThreadPool : public IExecutor
{
public:
    explicit StdThreadPool(std::size_t threads = 0)
    {
        if (threads == 0)
            threads = std::max<unsigned>(1u, std::thread::hardware_concurrency());
        workers_.reserve(threads);
        for (std::size_t i = 0; i < threads; ++i)
            workers_.emplace_back([this] { Worker(); });
    }
    ~StdThreadPool() override
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_)
            if (t.joinable())
                t.join();
    }
    StdThreadPool(const StdThreadPool&)            = delete;
    StdThreadPool& operator=(const StdThreadPool&) = delete;

    void Submit(std::function<void()> task) override
    {
        {
            std::lock_guard<std::mutex> lk(mu_);
            queue_.push_back(std::move(task));
        }
        cv_.notify_one();
    }
    size_t QueueDepth() const override
    {
        std::lock_guard<std::mutex> lk(mu_);
        return queue_.size();
    }

private:
    void Worker()
    {
        for (;;)
        {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(mu_);
                cv_.wait(lk, [&] { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty())
                    return;
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            try { task(); }
            catch (...) { /* worker swallows; the promise carries it */ }
        }
    }

    mutable std::mutex                mu_;
    std::condition_variable           cv_;
    std::deque<std::function<void()>> queue_;
    std::vector<std::thread>          workers_;
    bool                              stop_ = false;
};

// BSThreadPoolExecutor is defined at the bottom of this header, after the
// inlined BS::thread_pool body (it needs the complete type as a member).

// ----------------------------------------------------------------------
//  Trace + observer
// ----------------------------------------------------------------------
// One recorded event in a flow's trace. The trace is the ordered list of
// everything that happened during a flow; OnFlowEnded receives the whole
// vector - that is what makes "how far did it get before failing" answerable.
struct TraceEntry
{
    enum class Kind { StepTransition, AsyncCompletion };
    Kind        kind;           // which of the two timing fields applies
    int         ordinal = 0;    // step number within the flow
    std::string name;           // step name or async job label
    Duration    cpu_time  = {}; // StepTransition: main-thread time
    Duration    wait_time = {}; // AsyncCompletion: pool wait time
    bool        had_error = false;
    bool        timed_out = false;
    StepAction  action    = StepAction::Stay; // StepTransition: what the step returned
};

// Counters that persist across runs of a flow on the same module. max_seen_
// length drives the "reached #N/#M" log: N this run, M the longest ever.
struct FlowStats
{
    int         last_success_length = 0;
    int         max_seen_length     = 0; // the "#M" denominator
    std::size_t success_count       = 0;
    std::size_t fail_count          = 0;
};

// Every log line / metric the framework produces is funnelled through one of
// these hooks - the framework itself never touches std::cout. Subclass and
// install with uniflow::SetObserver; override only the events you care about.
class IUniflowObserver
{
public:
    virtual ~IUniflowObserver() = default;

    // Fired once when StartFlow() arms a fresh flow on `obj`. Called on the
    // pump thread driving that module, before any step runs. Use it to open
    // per-flow log scopes or reset per-flow counters.
    virtual void OnFlowStarted(std::string_view /*obj*/) {}

    // Fired immediately after every step function returns. `step` is the step
    // name (UF_NEXT-captured), `ordinal` is the 1-based step index within the
    // current flow, `cpu` is the wall time the step held the pump thread.
    // Use for per-step tracing and for hot-path profiling.
    virtual void OnStepRan(std::string_view /*obj*/, std::string_view /*step*/,
                           int /*ordinal*/, Duration /*cpu*/) {}

    // Fired right after UF_ASYNC successfully enqueues `job` to a pool. `step`
    // is the step that submitted it. Use to correlate "what kicked off this
    // worker" in logs; no result is available yet.
    virtual void OnAsyncSubmitted(std::string_view /*obj*/, std::string_view /*step*/,
                                  std::string_view /*job*/) {}

    // Fired once per async job, after the worker either returned a value,
    // threw, or missed its deadline. `wait` is the elapsed pool wait (submit
    // to completion). `had_error` is set if the worker threw; `timed_out` is
    // set if AsyncOpts::timeout was exceeded.
    virtual void OnAsyncCompleted(std::string_view /*obj*/, std::string_view /*job*/,
                                  Duration /*wait*/, bool /*had_error*/,
                                  bool /*timed_out*/) {}

    // Fired when a single step held the pump thread longer than
    // Config::slow_cpu_threshold. This is the single-thread-protection alarm:
    // any step that trips it is starving every other module on this runtime.
    virtual void OnSlowCpuStep(std::string_view /*obj*/, std::string_view /*step*/,
                               Duration /*cpu*/) {}

    // Fired at most once per async job, when its in-flight time crosses
    // AsyncOpts::slow_warn_after. The job is still pending; this is an early
    // signal, not a failure. Re-armed only on the next submission.
    virtual void OnSlowAsync(std::string_view /*obj*/, std::string_view /*job*/,
                             Duration /*wait_so_far*/) {}

    // Fired once when a flow leaves the running state via Done or Fail.
    // `terminal_action` distinguishes the two; `reached_ordinal` is the last
    // step index seen; `trace` is the ordered list of every step transition
    // and async completion. `wall_clock` = total flow duration; `total_cpu` =
    // summed pump-thread time across all its steps; `total_async_wait` =
    // summed time the flow spent gated on async jobs. `stats` is the running
    // per-module record (success/fail counts, max flow length ever seen).
    virtual void OnFlowEnded(std::string_view /*obj*/, StepAction /*terminal_action*/,
                             int /*reached_ordinal*/,
                             const std::vector<TraceEntry>& /*trace*/,
                             Duration /*wall_clock*/, Duration /*total_cpu*/,
                             Duration /*total_async_wait*/,
                             const FlowStats& /*stats*/) {}
};

// Default observer - pretty-prints to stdout. Thread-safe: every runtime's
// pump thread may call into it.
class ConsoleObserver : public IUniflowObserver
{
public:
    void OnFlowStarted(std::string_view obj) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << obj << "] flow ENTER\n";
    }
    void OnStepRan(std::string_view obj, std::string_view step,
                   int ordinal, Duration cpu) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << obj << "]   #" << pad2(ordinal) << " " << step
                  << "  cpu=" << fmt_ms(cpu) << "\n";
    }
    void OnAsyncSubmitted(std::string_view obj, std::string_view step,
                          std::string_view job) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << obj << "]        +- submit async " << job
                  << " (from " << step << ")\n";
    }
    void OnAsyncCompleted(std::string_view obj, std::string_view job,
                          Duration wait, bool had_error, bool timed_out) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << obj << "]        +- async " << job
                  << " wait=" << fmt_ms(wait);
        if (timed_out)      std::cout << "  [TIMEOUT]";
        else if (had_error) std::cout << "  [ERROR]";
        else                std::cout << "  [ok]";
        std::cout << "\n";
    }
    void OnSlowCpuStep(std::string_view obj, std::string_view step,
                       Duration cpu) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << obj << "]   [WARN] SLOW CPU step " << step
                  << " took " << fmt_ms(cpu) << " on the pump thread\n";
    }
    void OnSlowAsync(std::string_view obj, std::string_view job,
                     Duration wait_so_far) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << obj << "]   [WARN] SLOW ASYNC " << job
                  << " still pending after " << fmt_ms(wait_so_far) << "\n";
    }
    void OnFlowEnded(std::string_view obj, StepAction terminal_action,
                     int reached_ordinal, const std::vector<TraceEntry>& /*trace*/,
                     Duration wall_clock, Duration total_cpu,
                     Duration total_async_wait, const FlowStats& stats) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << obj << "] flow "
                  << (terminal_action == StepAction::Done ? "DONE" : "FAILED")
                  << "  reached #" << pad2(reached_ordinal);
        if (stats.max_seen_length)
            std::cout << "/#" << pad2(stats.max_seen_length);
        std::cout << "  wall=" << fmt_ms(wall_clock)
                  << "  cpu=" << fmt_ms(total_cpu)
                  << "  async=" << fmt_ms(total_async_wait) << "\n";
    }

private:
    static std::string pad2(int v)
    {
        std::ostringstream os;
        os << std::setw(2) << std::setfill('0') << v;
        return os.str();
    }
    static std::string fmt_ms(Duration d)
    {
        std::ostringstream os;
        os << std::fixed << std::setprecision(2) << to_ms(d) << "ms";
        return os.str();
    }
    std::mutex mu_;
};

// ----------------------------------------------------------------------
//  Config - global, set once before the first module/flow via uniflow::Configure
// ----------------------------------------------------------------------
struct Config
{
    // A step holding the pump thread longer than this trips OnSlowCpuStep -
    // the single-thread-protection alarm.
    Duration slow_cpu_threshold = std::chrono::milliseconds(1);
    // How long a pump cv-waits when every module on it is gated (no work).
    // With the cv-wake path this is only the timer resolution for Wait(d)
    // expiry when no external Notify() arrives - async completion and new
    // flows wake the pump immediately.
    Duration idle_sleep = std::chrono::milliseconds(1);
    // Default polling cadence for the no-arg Wait() helper. Picked once at
    // configure time; pass an explicit Wait(d) only when a step needs a
    // different rhythm. Steps that re-enter on this default never need to
    // think about polling - external Notify() will wake the pump anyway.
    Duration default_step_poll = std::chrono::milliseconds(20);
};

// ----------------------------------------------------------------------
//  detail - the hidden machinery: runtime, pump threads, the static hub
// ----------------------------------------------------------------------
namespace detail
{

// Set once per pump thread to the index of the runtime it drives; -1 on any
// other thread. GetInst()/inst() use it to assert that cross-module access
// stays within one runtime (one pump thread) - see DESIGN.md Sec.5-10.
inline thread_local int t_runtime_idx = -1;

// Uniflow<Derived> is a template, so a runtime cannot hold "all modules"
// directly - each module is a different type. IUniflowObject is the
// non-template base they share, so the pump can drive them uniformly.
class IUniflowObject
{
public:
    virtual ~IUniflowObject()                   = default;
    virtual bool IsIdle() const                 = 0; // no flow running?
    // tick; ran a step? `force_wake` makes the module ignore its in-flight
    // Wait(d) gate this round - the pump sets it when an external Notify()
    // told us the world changed, so the step should re-evaluate its
    // condition right away rather than wait the polling interval out.
    virtual bool ExecuteOnce(IUniflowObserver&, bool force_wake) = 0;
};

// One runtime == one pump thread. Created and owned by Hub; never by the user.
class UniflowRuntime
{
public:
    explicit UniflowRuntime(int index) : index_(index)
    {
        pump_ = std::thread([this] { pump_loop(); });
    }
    ~UniflowRuntime()
    {
        stop_.store(true, std::memory_order_relaxed);
        Notify(); // wake the pump out of its idle wait so it can observe stop_
        if (pump_.joinable())
            pump_.join();
    }
    UniflowRuntime(const UniflowRuntime&)            = delete;
    UniflowRuntime& operator=(const UniflowRuntime&) = delete;

    int index() const { return index_; }

    // Register / unregister a module with this runtime's pump. A module's base
    // ctor attaches; its base dtor detaches. The pump holds objects_mu_ for the
    // whole round, so detach (from a non-pump thread) lands strictly between
    // rounds - the pump never touches a module mid-destruction.
    void attach(IUniflowObject* m)
    {
        std::lock_guard<std::recursive_mutex> lk(objects_mu_);
        objects_.push_back(m);
    }
    void detach(IUniflowObject* m)
    {
        std::lock_guard<std::recursive_mutex> lk(objects_mu_);
        objects_.erase(std::remove(objects_.begin(), objects_.end(), m),
                       objects_.end());
    }

    // Wake the pump out of its idle wait at once. Called by:
    //   - any async worker right after it has set the result on its promise,
    //   - StartFlow() when a module transitions from idle to active,
    //   - the runtime destructor (so stop_ is observed without delay),
    //   - external signallers via uniflow::NotifyAll() (e.g. an IRQ-style
    //     callback that just flipped a condition some step is waiting on).
    // Cheap and lock-free in the steady state - if the pump is busy running
    // a round, the only effect is that the flag is already true.
    //
    // Also sets the external-wake flag so the NEXT round will bypass every
    // module's Wait(d) gate once: a step that returned Wait() will be given
    // the chance to re-evaluate its condition immediately instead of
    // sleeping its polling interval out.
    void Notify()
    {
        ext_wake_observed_.store(true, std::memory_order_release);
        {
            std::lock_guard<std::mutex> lk(wake_mu_);
            wake_pending_flag_ = true;
        }
        wake_cv_.notify_one();
    }
    // Read-and-clear the external-wake flag. Used by pump_loop to decide
    // whether to ask every module to bypass its Wait(d) gate this round.
    bool consume_external_wake()
    {
        return ext_wake_observed_.exchange(false, std::memory_order_acquire);
    }

private:
    void pump_loop(); // defined after Hub (it reads Hub's observer + config)

    int                          index_;
    std::recursive_mutex         objects_mu_; // recursive: a step may attach a
    std::vector<IUniflowObject*> objects_;    // freshly lazy-created module
    std::atomic<bool>            stop_{false};

    // Wake channel: pump sleeps on wake_cv_ at the end of a no-work round;
    // any external event (async completion, new flow) calls Notify().
    std::mutex                   wake_mu_;
    std::condition_variable      wake_cv_;
    bool                         wake_pending_flag_ = false;
    // Sticky flag set by Notify(), consumed once per pump round. While true,
    // ExecuteOnce is told to bypass each module's Wait(d) gate so steps that
    // are polling on an external condition get a chance to re-evaluate at
    // once. Atomic because Notify() runs on arbitrary threads.
    std::atomic<bool>            ext_wake_observed_{false};

    std::thread                  pump_;
};

// The static hub: owns the runtimes, the executors, the observer, the config.
// Lazily creates 1 runtime if uniflow::Init() was never called. A Meyers
// singleton - and the only one in the framework.
class Hub
{
public:
    static Hub& get()
    {
        static Hub h;
        return h;
    }

    // -- opt-in setup; all must run before the first module/flow exists --
    void init(int n)
    {
        std::lock_guard<std::mutex> lk(mu_);
        require_not_started("uniflow::Init()");
        desired_ = (n < 1) ? 1 : n;
    }
    void configure(const Config& c)
    {
        std::lock_guard<std::mutex> lk(mu_);
        require_not_started("uniflow::Configure()");
        cfg_ = c;
    }
    void register_executor(std::string name, std::shared_ptr<IExecutor> e)
    {
        std::lock_guard<std::mutex> lk(mu_);
        require_not_started("uniflow::RegisterExecutor()");
        executors_[std::move(name)] = std::move(e);
    }
    void set_observer(std::unique_ptr<IUniflowObserver> o)
    {
        std::lock_guard<std::mutex> lk(mu_);
        require_not_started("uniflow::SetObserver()");
        observer_ = std::move(o);
    }

    // -- used by modules at construction / async submission --
    UniflowRuntime& runtime(int idx)
    {
        boot();
        if (idx < 0 || idx >= static_cast<int>(runtimes_.size()))
            throw std::out_of_range(
                "uniflow: runtime index " + std::to_string(idx)
                + " out of range [0," + std::to_string(runtimes_.size()) + ")");
        return *runtimes_[static_cast<std::size_t>(idx)];
    }
    int runtime_count()
    {
        boot();
        return static_cast<int>(runtimes_.size());
    }
    // Wake every runtime's pump out of its cv-wait at once. Safe to call
    // from any thread - it does not touch module state, only the wake
    // channel. Public surface goes through uniflow::NotifyAll() below.
    void notify_all_runtimes()
    {
        // No boot() here: if no runtime has ever been created there is
        // nothing to wake, and we do not want to lazy-spin pumps for the
        // sake of a wake.
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& rt : runtimes_)
            if (rt) rt->Notify();
    }
    IExecutor& executor(const std::string& name)
    {
        boot();
        auto it = executors_.find(name);
        if (it == executors_.end())
            throw std::runtime_error("uniflow: unknown executor '" + name + "'");
        return *it->second;
    }
    IUniflowObserver& observer() { return *observer_; }
    const Config&     config() const { return cfg_; }

private:
    Hub() = default;
    ~Hub()
    {
        // Destroy the runtimes first - stops and joins every pump thread -
        // while observer_ and cfg_ are still alive for any final pump round.
        runtimes_.clear();
    }

    void require_not_started(const char* who)
    {
        if (started_)
            throw std::logic_error(std::string(who)
                + " must be called before any module or flow is created");
    }

    // First touch of any runtime/executor freezes setup and spins up the pumps.
    void boot()
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (started_)
            return;
        started_ = true;
        if (!observer_)
            observer_ = std::make_unique<ConsoleObserver>();
        if (executors_.find("default") == executors_.end())
            executors_["default"] = std::make_shared<StdThreadPool>(0);
        runtimes_.reserve(static_cast<std::size_t>(desired_));
        for (int i = 0; i < desired_; ++i)
            runtimes_.push_back(std::make_unique<UniflowRuntime>(i));
    }

    std::mutex                                            mu_;
    bool                                                  started_  = false;
    int                                                   desired_  = 1;
    Config                                                cfg_;
    std::map<std::string, std::shared_ptr<IExecutor>>     executors_;
    std::unique_ptr<IUniflowObserver>                     observer_;
    // Declared last -> destroyed first: pumps join before observer_/cfg_ die.
    std::vector<std::unique_ptr<UniflowRuntime>>          runtimes_;
};

// The pump loop - the single thread that drives one runtime. Each round ticks
// every non-idle module once (round-robin: no module starves another). If the
// round did real work, loop again at once; if nothing was active, sleep on the
// wake condition variable - external events (async completion, StartFlow) call
// Notify() to wake the pump immediately, so response latency is not bounded by
// Config::idle_sleep. idle_sleep is now only the timer resolution for Wait(d)
// expiry when no external event is in flight.
inline void UniflowRuntime::pump_loop()
{
    t_runtime_idx = index_;
    for (;;)
    {
        if (stop_.load(std::memory_order_relaxed))
            return;
        // Consume the external-wake flag ONCE per round. If set, every
        // module's Wait(d) gate is bypassed for this round so steps
        // polling on an external condition get to re-evaluate at once.
        bool force_wake = consume_external_wake();
        bool ran_step   = false;
        {
            // Held for the whole round. Index-based loop so a module that a
            // running step lazily creates (push_back) is tolerated; idle
            // newcomers are simply skipped this round.
            std::lock_guard<std::recursive_mutex> lk(objects_mu_);
            for (std::size_t i = 0; i < objects_.size(); ++i)
            {
                IUniflowObject* o = objects_[i];
                if (o->IsIdle())
                    continue;
                if (o->ExecuteOnce(Hub::get().observer(), force_wake))
                    ran_step = true;
            }
        }
        // No module ran a step this round - every active one is gated on a
        // pending async or a Wait(). Wait on the cv until either:
        //   - an external Notify() arrives (async completion, new flow), or
        //   - idle_sleep elapses (re-check Wait(d) timers).
        if (!ran_step)
        {
            std::unique_lock<std::mutex> lk(wake_mu_);
            wake_cv_.wait_for(lk, Hub::get().config().idle_sleep,
                              [this] { return wake_pending_flag_; });
            wake_pending_flag_ = false;
        }
    }
}

} // namespace detail

// ----------------------------------------------------------------------
//  Public setup surface - all optional, all before the first module/flow
// ----------------------------------------------------------------------

// Create N runtimes (N pump threads) instead of the lazy default of 1. Opt-in:
// most programs never call this. Must run before any module/flow is created.
inline void Init(int runtime_count)
{
    detail::Hub::get().init(runtime_count);
}

// Override the global Config (slow-CPU threshold, idle sleep).
inline void Configure(const Config& cfg)
{
    detail::Hub::get().configure(cfg);
}

// Register a named thread pool, selectable per call via AsyncOpts::executor_name.
// A pool named "default" is created automatically if none is registered.
inline void RegisterExecutor(std::string name, std::shared_ptr<IExecutor> exec)
{
    detail::Hub::get().register_executor(std::move(name), std::move(exec));
}

// Replace the default ConsoleObserver with your own logger / metrics sink.
inline void SetObserver(std::unique_ptr<IUniflowObserver> observer)
{
    detail::Hub::get().set_observer(std::move(observer));
}

// Wake every runtime's pump out of its cv-wait at once. Use from a
// signaller that just flipped a condition some step is waiting on
// (e.g. an IRQ-style callback that sets a hardware-ready flag): without
// this call, the step would not re-evaluate the predicate until its
// Wait()'s timer expires. Safe to call from any thread; cheap when no
// pump is sleeping.
inline void NotifyAll()
{
    detail::Hub::get().notify_all_runtimes();
}

// ----------------------------------------------------------------------
//  UFTimer - "wait for a condition with a deadline" helper for steps.
//
//  Two ways to use it. Idiomatic (procedural, no switch):
//
//      StepResult OnInit_WaitHwReady(UFTimer t) {
//          if (LineState::Stop())      return Done();
//          if (t.TimedOut(1000ms))     return Fail();
//          if (!Hw::ReadySignal())     return Wait();
//          return UF_NEXT(OnInit_NextStep);
//      }
//
//  Or compact (one-shot OnWait + switch when you want all three branches
//  visible at a glance):
//
//      switch (t.OnWait(Hw::ReadySignal, 1000ms)) {
//      case UFTimer::Done:    return UF_NEXT(OnInit_NextStep);
//      case UFTimer::Timeout: return Fail();
//      case UFTimer::Waiting: return Wait();
//      }
//
//  Construction snapshots the start time, so re-entries of the same step
//  (after Wait()) keep counting from the original arming. Value semantics:
//  pass it through UF_NEXT by value; copies share no mutable state.
//
//  Note on responsiveness: the condition is re-evaluated only when the
//  pump invokes the step. If the step returned Wait() and the predicate
//  becomes true mid-wait, the step does not re-run until the wait expires -
//  unless whoever flipped the predicate also called uniflow::NotifyAll() to
//  wake the pump (uniflow's async path does this automatically; manual
//  signallers must do it themselves).
// ----------------------------------------------------------------------
class UFTimer
{
public:
    enum Result : uint8_t { Waiting, Done, Timeout };

    UFTimer() : armed_at_(Clock::now()) {}

    // Re-arm: the deadline is measured from this call onward.
    void Restart() { armed_at_ = Clock::now(); }

    Duration Elapsed() const { return Clock::now() - armed_at_; }

    // Shorthand for the if-pattern: "has the deadline passed yet?"
    bool TimedOut(Duration deadline) const { return Elapsed() >= deadline; }

    // Compact one-shot: evaluate `condition` once and classify.
    //   true            -> Done
    //   timeout reached -> Timeout
    //   else            -> Waiting
    template <class Predicate>
    Result OnWait(Predicate condition, Duration timeout) const
    {
        if (condition()) return Done;
        if (TimedOut(timeout)) return Timeout;
        return Waiting;
    }

private:
    TimePoint armed_at_;
};

// ----------------------------------------------------------------------
//  Uniflow<Derived> - the CRTP base your modules inherit
//
//  CRTP = Curiously Recurring Template Pattern: a module inherits from the base
//  parameterised on *itself* - `class Arm : Uniflow<Arm>`. That hands the base
//  the derived type at compile time, so step functions are pointers-to-member
//  of the derived class and run with no virtual dispatch.
//
//  Put UF_USES_UNIFLOW(Cls) - or UF_SINGLETON(Cls) - once in the class body.
// ----------------------------------------------------------------------
template <class Derived>
class Uniflow : public detail::IUniflowObject
{
public:
    // What a step returns: an intent, not a state change. The runtime reads
    // `action` and, for Advance, calls `next_fn` next tick. `next_name` is
    // the step's name string, captured by UF_NEXT(#fn) purely for the trace.
    //
    // next_fn is a std::function<StepResult()> built by UF_NEXT - it has
    // already captured any forwarded args, so the pump just invokes it with
    // no arguments. The entry step and every subsequent step go through the
    // exact same path; there is no "entry vs steady" split anymore.
    struct StepResult
    {
        StepAction                  action    = StepAction::Stay;
        std::function<StepResult()> next_fn;          // set only when Advance
        const char*                 next_name = nullptr; // step name for the log
        Duration                    delay     = {};      // set only when Wait
    };

    // -- Constructors (inherited into Derived via UF_USES_UNIFLOW) --
    //   Uniflow()                       anonymous, runtime 0
    //   Uniflow("name")                 named,     runtime 0
    //   Uniflow(2)                      anonymous, runtime 2
    //   Uniflow("name", 2)              named,     runtime 2
    // A type may have at most one *anonymous* instance (auto-named after the
    // class); a second one throws - name them. Names are unique per type only.
    Uniflow() : Uniflow(nullptr, 0) {}
    explicit Uniflow(const char* name) : Uniflow(name, 0) {}
    explicit Uniflow(int runtime_index) : Uniflow(nullptr, runtime_index) {}

    Uniflow(const char* name, int runtime_index)
        : runtime_index_(runtime_index)
    {
        const bool anon = (name == nullptr);
        instance_name_  = anon ? Derived::uf_class_name_() : std::string(name);
        is_anonymous_   = anon;

        // Claim identity in this type's registry (see GetInst / inst()).
        {
            Registry& reg = registry();
            std::lock_guard<std::mutex> lk(reg.mu);
            if (anon && reg.anon_used)
                throw std::logic_error(
                    std::string("uniflow: a second anonymous instance of '")
                    + Derived::uf_class_name_()
                    + "' - give your instances explicit names");
            if (!reg.by_name.emplace(instance_name_,
                                     static_cast<Derived*>(this)).second)
                throw std::logic_error(
                    std::string("uniflow: duplicate instance name '")
                    + instance_name_ + "' for type "
                    + Derived::uf_class_name_());
            if (anon)
                reg.anon_used = true;
        }

        // Join a runtime's pump. Safe even though Derived is not fully built:
        // the module is idle (no flow) until StartFlow(), and the pump skips it.
        runtime_ = &detail::Hub::get().runtime(runtime_index);
        runtime_->attach(this);
    }

    ~Uniflow() override
    {
        // Destroying a module mid-flow is a use-after-free hazard (the pump may
        // be in its step). Contract: destroy modules only when idle.
        assert(!flow_running_ && "uniflow: module destroyed while a flow runs");
        if (runtime_)
            runtime_->detach(this); // blocks until the current round ends
        Registry& reg = registry();
        std::lock_guard<std::mutex> lk(reg.mu);
        reg.by_name.erase(instance_name_);
        if (is_anonymous_)
            reg.anon_used = false;
    }

    Uniflow(const Uniflow&)            = delete;
    Uniflow& operator=(const Uniflow&) = delete;

    // -- IUniflowObject --
    bool IsIdle() const override
    {
        std::lock_guard<std::mutex> lk(op_mu_);
        return !flow_running_;
    }
    bool ExecuteOnce(IUniflowObserver& obs, bool force_wake) override;

    // -- Public control surface --

    // StartFlow a flow at entry step `fn`, forwarding `args` to it. Any step
    // (entry or steady) may take parameters: UF_NEXT(fn, args...) captures
    // them into the next step fn the same way StartFlow does for the entry.
    // Returns false if a flow is already running. Thread-safe - serialised
    // with the pump by the per-module mutex; writes the caller makes before
    // StartFlow() are visible to the flow.
    template <class... Params, class... Args>
    bool StartFlow(StepResult (Derived::*fn)(Params...), Args&&... args)
    {
        std::lock_guard<std::mutex> lk(op_mu_);
        if (flow_running_)
            return false;

        Derived* self = static_cast<Derived*>(this);
        // Decay-copy the args into the step fn so the entry step can be re-run
        // (a Stay) and never reaches back into the caller's storage.
        curr_fn_ =
            [self, fn,
             tup = std::tuple<std::decay_t<Args>...>(std::forward<Args>(args)...)]
            () mutable -> StepResult
            {
                return std::apply(
                    [self, fn](auto&... a) -> StepResult
                    { return (self->*fn)(a...); },
                    tup);
            };

        flow_running_         = true;
        curr_step_name_       = "(entry)";
        curr_ordinal_         = 0;
        flow_started_at_      = Clock::now();
        total_cpu_            = {};
        total_async_          = {};
        async_pending_        = false;
        slow_warned_          = false;
        wake_pending_         = false;
        trace_.clear();
        flow_started_pending_ = true;

        // Wake the pump in case it was idle-waiting. Without this, the new
        // flow would not start running until the pump's next idle_sleep tick.
        if (runtime_)
            runtime_->Notify();
        return true;
    }

    // Block the calling thread until the running flow finishes (returns at
    // once if already idle). Call from the OWNING thread - never from inside
    // a step. Named WaitUntilIdle (not Wait) so it cannot be confused with
    // the protected static Wait()/Wait(d) step-result helpers that are in
    // scope inside every step body.
    void WaitUntilIdle()
    {
        std::unique_lock<std::mutex> lk(op_mu_);
        idle_cv_.wait(lk, [this] { return !flow_running_; });
    }

    const std::string& InstanceName() const { return instance_name_; }
    int                RuntimeIndex() const { return runtime_index_; }

    // -- Cross-module access --
    // Fetch another module of this type by name (or the anonymous one). Safe
    // only between modules on the *same* runtime - a debug assert guards it.
    static Derived& GetInst(const char* name)
    {
        Registry& reg = registry();
        std::lock_guard<std::mutex> lk(reg.mu);
        auto it = reg.by_name.find(name ? name : "");
        if (it == reg.by_name.end())
            throw std::logic_error(
                std::string("uniflow: no instance '") + (name ? name : "")
                + "' of type " + Derived::uf_class_name_());
        Derived* p = it->second;
        assert((detail::t_runtime_idx < 0
                || detail::t_runtime_idx == p->runtime_index_)
               && "uniflow: cross-runtime GetInst - access is unsafe across "
                  "pump threads");
        return *p;
    }
    static Derived& GetInst() { return GetInst(Derived::uf_class_name_()); }

protected:
    // -- Helpers your step functions return --
    static StepResult Stay() { return {StepAction::Stay, {}, nullptr, {}}; }
    static StepResult Done() { return {StepAction::Done, {}, nullptr, {}}; }
    static StepResult Fail() { return {StepAction::Fail, {}, nullptr, {}}; }
    // Re-run this same step, but not before `d` has elapsed. The paced
    // counterpart of Stay() - for "poll again soon" or "let motion advance"
    // without the pump busy-spinning.
    static StepResult Wait(Duration d)
    {
        return {StepAction::Wait, {}, nullptr, d};
    }
    // No-arg overload: hold for the configured default poll cadence
    // (Config::default_step_poll). Use this when the step does not care
    // about polling rhythm and an external Notify() will wake it anyway -
    // which is the common case for "wait until some condition flips."
    static StepResult Wait()
    {
        return Wait(detail::Hub::get().config().default_step_poll);
    }

    // Advance to step `Fn` (a member-function pointer baked in at compile
    // time as a non-type template parameter). Captures any forwarded args
    // into the step fn so the next step is invoked with the same arguments
    // the caller wrote in the UF_NEXT macro. Not called directly - go
    // through UF_NEXT(fn) or UF_NEXT(fn, arg1, arg2, ...).
    template <auto Fn, class... Args>
    StepResult Next_(const char* name, Args&&... args)
    {
        Derived* self = static_cast<Derived*>(this);
        if constexpr (sizeof...(Args) == 0)
        {
            // Fast path: no captured args, no tuple, no apply.
            return {StepAction::Advance,
                    [self]() -> StepResult { return (self->*Fn)(); },
                    name, {}};
        }
        else
        {
            auto tup = std::tuple<std::decay_t<Args>...>(
                std::forward<Args>(args)...);
            return {StepAction::Advance,
                    [self, tup = std::move(tup)]() mutable -> StepResult {
                        return std::apply(
                            [self](auto&... a) -> StepResult {
                                return (self->*Fn)(a...);
                            },
                            tup);
                    },
                    name, {}};
        }
    }

    // Submit a static function to a thread pool. Use UF_ASYNC / UF_ASYNC_OPT.
    template <auto Fn, class... Args>
    void SubmitAsync(const char* job_label, AsyncOpts opts, Args&&... args);

    // Read the result of the most-recent async submission. Call from the
    // continuation step (the one UF_NEXT named right after UF_ASYNC).
    template <class T>
    AsyncRef<T> AsyncResult()
    {
        return AsyncRef<T>{std::any_cast<T>(&last_async_value_),
                           last_async_exception_,
                           last_async_was_timeout_};
    }

private:
    // Per-type instance registry - distinct for each Uniflow<Derived>, since
    // CRTP makes each a separate class. Keyed by name; tracks the anonymous one.
    struct Registry
    {
        std::mutex                                mu;
        std::unordered_map<std::string, Derived*> by_name;
        bool                                      anon_used = false;
    };
    static Registry& registry()
    {
        static Registry r;
        return r;
    }

    // Reset to idle once a flow reaches Done / Fail.
    void ClearFlow()
    {
        flow_running_   = false;
        curr_fn_     = nullptr;
        curr_step_name_ = nullptr;
        curr_ordinal_   = 0;
        async_pending_  = false;
        slow_warned_    = false;
        wake_pending_   = false;
    }

    std::string             instance_name_;
    int                     runtime_index_ = 0;
    bool                    is_anonymous_  = false;
    detail::UniflowRuntime* runtime_       = nullptr;

    // Serialises external StartFlow()/WaitUntilIdle() against pump ExecuteOnce().
    // Mutable so the const IsIdle() can lock it.
    mutable std::mutex      op_mu_;
    std::condition_variable idle_cv_; // notified when a flow ends (for WaitUntilIdle)

    // -- Current position within the running flow --
    bool        flow_running_         = false;
    const char* curr_step_name_       = nullptr; // step name (for logs)
    int         curr_ordinal_         = 0;       // steps run so far this flow
    TimePoint   flow_started_at_      = {};
    Duration    total_cpu_            = {};      // summed pump-thread step time
    Duration    total_async_          = {};      // summed pool wait time
    bool        flow_started_pending_ = false;   // defer OnFlowStarted one tick
    bool        wake_pending_         = false;   // a Wait() delay is in effect
    TimePoint   wake_at_              = {};      // when the Wait()'d step re-runs

    // The current step, held as a callable std::function. The entry step (set by StartFlow)
    // and every subsequent step (set by Advance from UF_NEXT) live in this
    // single slot - the pump just invokes curr_fn_() with no arguments.
    std::function<StepResult()> curr_fn_;

    // -- Async slot: state of the one in-flight UF_ASYNC submission --
    bool                  async_pending_ = false;
    bool                  slow_warned_   = false;
    std::future<std::any> pending_fut_;
    const char*           pending_job_label_ = nullptr;
    AsyncOpts             pending_opts_;
    TimePoint             pending_submitted_at_;

    // -- Last async result - written on completion, read via AsyncResult<T>() --
    std::any           last_async_value_;
    std::exception_ptr last_async_exception_;
    bool               last_async_was_timeout_ = false;

    // -- Trace + cross-flow stats handed to OnFlowEnded --
    std::vector<TraceEntry> trace_;
    FlowStats               stats_;
};

// ----------------------------------------------------------------------
//  Uniflow<Derived> - out-of-line definitions (need detail::Hub complete)
// ----------------------------------------------------------------------

// ExecuteOnce advances this module by one tick. The pump calls it round-robin
// on every non-idle module. One tick does at most one of:
//   - nothing yet (an async job it waits on has not finished), or
//   - run exactly one step and act on what that step returned.
// It never blocks: a not-ready async job makes it return at once.
template <class Derived>
bool Uniflow<Derived>::ExecuteOnce(IUniflowObserver& obs, bool force_wake)
{
    // One lock for the whole tick, pairing with StartFlow()/Wait()'s lock.
    std::lock_guard<std::mutex> lk(op_mu_);

    // StartFlow() only set a flag; emit OnFlowStarted here so it stays ordered
    // with the step events the pump thread produces.
    if (flow_started_pending_)
    {
        obs.OnFlowStarted(instance_name_);
        flow_started_pending_ = false;
    }
    if (!flow_running_)
        return false; // idle - nothing to do

    // -- 1. Gate on pending async --
    // If a previous step submitted async work, do not run the continuation
    // step until that work finishes (or times out).
    if (async_pending_)
    {
        auto now       = Clock::now();
        auto elapsed   = now - pending_submitted_at_;
        // wait_for(0) just polls - it never blocks the pump thread.
        bool ready     = pending_fut_.wait_for(std::chrono::seconds(0))
                             == std::future_status::ready;
        bool timed_out = (elapsed > pending_opts_.timeout);

        // Still running and within deadline: maybe emit a one-time slow
        // warning, then leave. This return is an implicit Stay.
        if (!ready && !timed_out)
        {
            if (pending_opts_.slow_warn_after
                && elapsed > *pending_opts_.slow_warn_after && !slow_warned_)
            {
                obs.OnSlowAsync(instance_name_, pending_job_label_, elapsed);
                slow_warned_ = true;
            }
            return false; // no step ran - the pump may sleep
        }

        // Ready, or blew its deadline -> move the outcome into the result slot.
        if (timed_out && !ready)
        {
            last_async_value_.reset();
            last_async_exception_ = std::make_exception_ptr(
                AsyncTimeout{pending_job_label_, elapsed});
            last_async_was_timeout_ = true;
        }
        else
        {
            // future::get() rethrows whatever the worker threw - a throw
            // becomes a stored exception_ptr, never a pump-thread crash.
            try
            {
                last_async_value_       = pending_fut_.get();
                last_async_exception_   = nullptr;
                last_async_was_timeout_ = false;
            }
            catch (...)
            {
                last_async_value_.reset();
                last_async_exception_   = std::current_exception();
                last_async_was_timeout_ = false;
            }
        }
        obs.OnAsyncCompleted(instance_name_, pending_job_label_, elapsed,
                             last_async_exception_ != nullptr,
                             last_async_was_timeout_);

        // Record the wait as its own trace entry - kept separate from step
        // CPU time so "pump-thread cost" stays honest.
        TraceEntry te;
        te.kind      = TraceEntry::Kind::AsyncCompletion;
        te.ordinal   = curr_ordinal_;
        te.name      = pending_job_label_ ? pending_job_label_ : "";
        te.wait_time = elapsed;
        te.had_error = last_async_exception_ != nullptr;
        te.timed_out = last_async_was_timeout_;
        trace_.push_back(std::move(te));
        total_async_ += elapsed;

        async_pending_ = false;
        slow_warned_   = false;
        // Slot filled; fall through to run the continuation step.
    }

    // -- 1b. Honour a Wait(): hold the step until its delay elapses --
    //    unless the pump told us an external Notify() happened this round,
    //    in which case the polling condition might already be true and we
    //    must give the step a chance to re-evaluate at once.
    if (wake_pending_)
    {
        if (!force_wake && Clock::now() < wake_at_)
            return false; // not due yet - no step ran, the pump may sleep
        wake_pending_ = false;
    }

    // -- 2. Run the step (timed) --
    // Both the entry step (set by StartFlow) and every steady step (set by
    // an Advance from UF_NEXT) live in curr_fn_ - no entry/steady split.
    const char* step_name = curr_step_name_;
    auto        t0        = Clock::now();
    StepResult  r         = curr_fn_();
    auto        cpu_dt    = Clock::now() - t0;

    total_cpu_ += cpu_dt;
    curr_ordinal_++;

    {
        TraceEntry te;
        te.kind     = TraceEntry::Kind::StepTransition;
        te.ordinal  = curr_ordinal_;
        te.name     = step_name ? step_name : "";
        te.cpu_time = cpu_dt;
        te.action   = r.action;
        trace_.push_back(std::move(te));
    }

    obs.OnStepRan(instance_name_, step_name ? step_name : "",
                  curr_ordinal_, cpu_dt);

    // A step shares the one pump thread with every other module - if it
    // overran the threshold, raise the core single-thread-protection alarm.
    if (cpu_dt > detail::Hub::get().config().slow_cpu_threshold)
        obs.OnSlowCpuStep(instance_name_, step_name ? step_name : "", cpu_dt);

    // If this very step called UF_ASYNC, async_pending_ is now set.
    if (async_pending_)
        obs.OnAsyncSubmitted(instance_name_, step_name ? step_name : "",
                             pending_job_label_ ? pending_job_label_ : "");

    // -- 3. Apply the StepResult --
    switch (r.action)
    {
    case StepAction::Stay:
        // Cursor unchanged: curr_fn_ re-runs next tick.
        break;
    case StepAction::Wait:
        // Cursor unchanged, but hold the re-run until the delay elapses.
        wake_pending_ = true;
        wake_at_      = Clock::now() + r.delay;
        break;
    case StepAction::Advance:
        // The next step fn has captured its bound arguments (if any) already.
        curr_fn_     = std::move(r.next_fn);
        curr_step_name_ = r.next_name;
        break;
    case StepAction::Done:
    case StepAction::Fail:
    {
        if (r.action == StepAction::Done)
        {
            stats_.success_count++;
            stats_.last_success_length = curr_ordinal_;
            if (curr_ordinal_ > stats_.max_seen_length)
                stats_.max_seen_length = curr_ordinal_;
        }
        else
        {
            stats_.fail_count++;
        }
        obs.OnFlowEnded(instance_name_, r.action, curr_ordinal_, trace_,
                        Clock::now() - flow_started_at_, total_cpu_,
                        total_async_, stats_);
        ClearFlow();
        idle_cv_.notify_all(); // wake any Wait()
        break;
    }
    }
    return true; // a real step ran this tick
}

// SubmitAsync hands a job to a thread pool and arms the async slot. `Fn` is a
// non-type template parameter - the actual function baked in at compile time,
// so the worker lambda calls it with zero indirection. UF_ASYNC fills it in.
template <class Derived>
template <auto Fn, class... Args>
void Uniflow<Derived>::SubmitAsync(const char* job_label, AsyncOpts opts,
                                   Args&&... args)
{
    // Compile-time safety gate: the target must NOT be a non-static member
    // function. A worker on another thread must never touch `this` -
    // everything it needs is passed explicitly through args.
    static_assert(!std::is_member_function_pointer_v<decltype(Fn)>,
                  "UF_ASYNC target must be a `static` member or free function. "
                  "Instance state is unsafe across threads - pass needed data "
                  "through args.");

    using R = std::invoke_result_t<decltype(Fn), Args...>;

    // promise/future is the one-shot channel from the worker back to the pump.
    auto promise          = std::make_shared<std::promise<std::any>>();
    pending_fut_          = promise->get_future();
    pending_job_label_    = job_label;
    pending_opts_         = std::move(opts);
    pending_submitted_at_ = Clock::now();
    async_pending_        = true;
    slow_warned_          = false;

    // Decay-copy the args into a value tuple - the worker can never reach
    // back into `this` even if the caller passed a member reference.
    auto tup = std::tuple<std::decay_t<Args>...>(std::forward<Args>(args)...);

    // Capture the runtime pointer so the worker can wake the pump as soon
    // as the result is set. Without this, the pump would only observe the
    // ready future on its next idle_sleep tick.
    detail::UniflowRuntime* rt = runtime_;

    detail::Hub::get().executor(pending_opts_.executor_name).Submit(
        [promise, tup = std::move(tup), rt]() mutable
        {
            try
            {
                if constexpr (std::is_void_v<R>)
                {
                    std::apply([](auto&&... a)
                               { Fn(std::forward<decltype(a)>(a)...); },
                               std::move(tup));
                    promise->set_value(std::any{});
                }
                else
                {
                    R result = std::apply(
                        [](auto&&... a)
                        { return Fn(std::forward<decltype(a)>(a)...); },
                        std::move(tup));
                    promise->set_value(std::any(std::move(result)));
                }
            }
            catch (...)
            {
                // Captured here, rethrown by future::get() on the pump thread.
                promise->set_exception(std::current_exception());
            }
            // Worker is done - either set_value or set_exception ran. Wake
            // the pump so it observes the ready future at once, not on the
            // next idle_sleep tick.
            if (rt) rt->Notify();
        });
}

} // namespace uniflow

// ----------------------------------------------------------------------
//  Convenience macros
//
//  Place UF_USES_UNIFLOW(MyClass) - or UF_SINGLETON(MyClass) - once at the top
//  of the class body. Both leave the class in a `private:` section, so add
//  `public:` before your entry steps. The macros capture, for every step /
//  worker function, both `&S::fn` (the pointer) and `#fn` (its name string) -
//  which is why step names appear in logs with no manual logging in your code.
// ----------------------------------------------------------------------

// For a module that may have several instances. Inherits the base constructors
// (so `MyClass m{"name"}` works) and grants the base template friendship.
#define UF_USES_UNIFLOW(Cls)                                                  \
  public:                                                                     \
    using uniflow::Uniflow<Cls>::Uniflow;                                     \
                                                                              \
  private:                                                                    \
    using S = Cls;                                                            \
    friend class ::uniflow::Uniflow<Cls>;                                     \
    static const char* uf_class_name_() { return #Cls; }

// For a single-instance module. Provides Cls::inst() and makes the class
// non-constructible elsewhere (the default constructor is private), so a
// stray second instance is a compile error rather than a runtime throw.
#define UF_SINGLETON(Cls)                                                     \
  public:                                                                     \
    static Cls& inst()                                                        \
    {                                                                         \
        static Cls the_instance_;                                             \
        return the_instance_;                                                 \
    }                                                                         \
                                                                              \
  private:                                                                    \
    Cls() = default;                                                          \
    using S = Cls;                                                            \
    friend class ::uniflow::Uniflow<Cls>;                                     \
    static const char* uf_class_name_() { return #Cls; }

// Advance to step `fn`, optionally passing arguments through. With no extra
// arguments it behaves like the original UF_NEXT(fn); with arguments, those
// values are captured by the next step fn and delivered to fn when the pump
// invokes it. The `, ##__VA_ARGS__` GNU comma-elision form drops the leading
// comma when nothing follows fn (GCC, Clang, and MSVC's preprocessor honour
// it - same idiom UF_ASYNC already uses).
#define UF_NEXT(fn, ...) \
    this->template Next_<&S::fn>(#fn, ##__VA_ARGS__)

// Submit static function `fn` to the "default" pool with default options.
// `, ##__VA_ARGS__` is the GNU comma-elision form: the leading comma is
// dropped when no extra args are passed. GCC, Clang, and the MSVC
// preprocessor all honour it - no special compiler flag needed.
#define UF_ASYNC(fn, ...)                                                     \
    this->template SubmitAsync<&S::fn>(#fn, ::uniflow::AsyncOpts{},           \
                                       ##__VA_ARGS__)

// Same as UF_ASYNC but with an explicit AsyncOpts (timeout / pool / warn).
#define UF_ASYNC_OPT(fn, opts, ...)                                           \
    this->template SubmitAsync<&S::fn>(#fn, (opts), ##__VA_ARGS__)

// ======================================================================
//  BS::thread_pool - bundled in-place (was a separate header).
//
//  Upstream: https://github.com/bshoshany/thread-pool
//  Author:   Barak Shoshany (baraksh@gmail.com)
//  Version:  4.1.0 (2024-03-22)
//  License:  MIT (full notice preserved in the /** ... */ block below)
//
//  If you use this library in published research, please cite:
//    Barak Shoshany, "A C++17 Thread Pool for High-Performance
//    Scientific Computing", doi:10.1016/j.softx.2024.101687,
//    SoftwareX 26 (2024) 101687, arXiv:2105.00613
// ======================================================================
/**
 * @file BS_thread_pool.hpp
 * @author Barak Shoshany (baraksh@gmail.com) (https://baraksh.com)
 * @version 4.1.0
 * @date 2024-03-22
 * @copyright Copyright (c) 2024 Barak Shoshany. Licensed under the MIT license. If you found this project useful, please consider starring it on GitHub! If you use this library in software of any kind, please provide a link to the GitHub repository https://github.com/bshoshany/thread-pool in the source code and documentation. If you use this library in published research, please cite it as follows: Barak Shoshany, "A C++17 Thread Pool for High-Performance Scientific Computing", doi:10.1016/j.softx.2024.101687, SoftwareX 26 (2024) 101687, arXiv:2105.00613
 *
 * @brief BS::thread_pool: a fast, lightweight, and easy-to-use C++17 thread pool library. This header file contains the main thread pool class and some additional classes and definitions. No other files are needed in order to use the thread pool itself.
 */

#ifndef __cpp_exceptions
    #define BS_THREAD_POOL_DISABLE_EXCEPTION_HANDLING
    #undef BS_THREAD_POOL_ENABLE_WAIT_DEADLOCK_CHECK
#endif

#include <chrono>             // std::chrono
#include <condition_variable> // std::condition_variable
#include <cstddef>            // std::size_t
#ifdef BS_THREAD_POOL_ENABLE_PRIORITY
    #include <cstdint>        // std::int_least16_t
#endif
#ifndef BS_THREAD_POOL_DISABLE_EXCEPTION_HANDLING
    #include <exception>      // std::current_exception
#endif
#include <functional>         // std::function
#include <future>             // std::future, std::future_status, std::promise
#include <memory>             // std::make_shared, std::make_unique, std::shared_ptr, std::unique_ptr
#include <mutex>              // std::mutex, std::scoped_lock, std::unique_lock
#include <optional>           // std::nullopt, std::optional
#include <queue>              // std::priority_queue (if priority enabled), std::queue
#ifdef BS_THREAD_POOL_ENABLE_WAIT_DEADLOCK_CHECK
    #include <stdexcept>      // std::runtime_error
#endif
#include <thread>             // std::thread
#include <type_traits>        // std::conditional_t, std::decay_t, std::invoke_result_t, std::is_void_v, std::remove_const_t (if priority enabled)
#include <utility>            // std::forward, std::move
#include <vector>             // std::vector

/**
 * @brief A namespace used by Barak Shoshany's projects.
 */
namespace BS {
// Macros indicating the version of the thread pool library.
#define BS_THREAD_POOL_VERSION_MAJOR 4
#define BS_THREAD_POOL_VERSION_MINOR 1
#define BS_THREAD_POOL_VERSION_PATCH 0

class thread_pool;

/**
 * @brief A type to represent the size of things.
 */
using size_t = std::size_t;

/**
 * @brief A convenient shorthand for the type of `std::thread::hardware_concurrency()`. Should evaluate to unsigned int.
 */
using concurrency_t = std::invoke_result_t<decltype(std::thread::hardware_concurrency)>;

#ifdef BS_THREAD_POOL_ENABLE_PRIORITY
/**
 * @brief A type used to indicate the priority of a task. Defined to be an integer with a width of (at least) 16 bits.
 */
using priority_t = std::int_least16_t;

/**
 * @brief A namespace containing some pre-defined priorities for convenience.
 */
namespace pr {
    constexpr priority_t highest = 32767;
    constexpr priority_t high = 16383;
    constexpr priority_t normal = 0;
    constexpr priority_t low = -16384;
    constexpr priority_t lowest = -32768;
} // namespace pr

    // Macros used internally to enable or disable the priority arguments in the relevant functions.
    #define BS_THREAD_POOL_PRIORITY_INPUT , const priority_t priority = 0
    #define BS_THREAD_POOL_PRIORITY_OUTPUT , priority
#else
    #define BS_THREAD_POOL_PRIORITY_INPUT
    #define BS_THREAD_POOL_PRIORITY_OUTPUT
#endif

/**
 * @brief A namespace used to obtain information about the current thread.
 */
namespace this_thread {
    /**
     * @brief A type returned by `BS::this_thread::get_index()` which can optionally contain the index of a thread, if that thread belongs to a `BS::thread_pool`. Otherwise, it will contain no value.
     */
    using optional_index = std::optional<size_t>;

    /**
     * @brief A type returned by `BS::this_thread::get_pool()` which can optionally contain the pointer to the pool that owns a thread, if that thread belongs to a `BS::thread_pool`. Otherwise, it will contain no value.
     */
    using optional_pool = std::optional<thread_pool*>;

    /**
     * @brief A helper class to store information about the index of the current thread.
     */
    class [[nodiscard]] thread_info_index
    {
        friend class BS::thread_pool;

    public:
        /**
         * @brief Get the index of the current thread. If this thread belongs to a `BS::thread_pool` object, it will have an index from 0 to `BS::thread_pool::get_thread_count() - 1`. Otherwise, for example if this thread is the main thread or an independent `std::thread`, `std::nullopt` will be returned.
         *
         * @return An `std::optional` object, optionally containing a thread index. Unless you are 100% sure this thread is in a pool, first use `std::optional::has_value()` to check if it contains a value, and if so, use `std::optional::value()` to obtain that value.
         */
        [[nodiscard]] optional_index operator()() const
        {
            return index;
        }

    private:
        /**
         * @brief The index of the current thread.
         */
        optional_index index = std::nullopt;
    }; // class thread_info_index

    /**
     * @brief A helper class to store information about the thread pool that owns the current thread.
     */
    class [[nodiscard]] thread_info_pool
    {
        friend class BS::thread_pool;

    public:
        /**
         * @brief Get the pointer to the thread pool that owns the current thread. If this thread belongs to a `BS::thread_pool` object, a pointer to that object will be returned. Otherwise, for example if this thread is the main thread or an independent `std::thread`, `std::nullopt` will be returned.
         *
         * @return An `std::optional` object, optionally containing a pointer to a thread pool. Unless you are 100% sure this thread is in a pool, first use `std::optional::has_value()` to check if it contains a value, and if so, use `std::optional::value()` to obtain that value.
         */
        [[nodiscard]] optional_pool operator()() const
        {
            return pool;
        }

    private:
        /**
         * @brief A pointer to the thread pool that owns the current thread.
         */
        optional_pool pool = std::nullopt;
    }; // class thread_info_pool

    /**
     * @brief A `thread_local` object used to obtain information about the index of the current thread.
     */
    inline thread_local thread_info_index get_index;

    /**
     * @brief A `thread_local` object used to obtain information about the thread pool that owns the current thread.
     */
    inline thread_local thread_info_pool get_pool;
} // namespace this_thread

/**
 * @brief A helper class to facilitate waiting for and/or getting the results of multiple futures at once.
 *
 * @tparam T The return type of the futures.
 */
template <typename T>
class [[nodiscard]] multi_future : public std::vector<std::future<T>>
{
public:
    // Inherit all constructors from the base class `std::vector`.
    using std::vector<std::future<T>>::vector;

    // The copy constructor and copy assignment operator are deleted. The elements stored in a `multi_future` are futures, which cannot be copied.
    multi_future(const multi_future&) = delete;
    multi_future& operator=(const multi_future&) = delete;

    // The move constructor and move assignment operator are defaulted.
    multi_future(multi_future&&) = default;
    multi_future& operator=(multi_future&&) = default;

    /**
     * @brief Get the results from all the futures stored in this `multi_future`, rethrowing any stored exceptions.
     *
     * @return If the futures return `void`, this function returns `void` as well. Otherwise, it returns a vector containing the results.
     */
    [[nodiscard]] std::conditional_t<std::is_void_v<T>, void, std::vector<T>> get()
    {
        if constexpr (std::is_void_v<T>)
        {
            for (std::future<T>& future : *this)
                future.get();
            return;
        }
        else
        {
            std::vector<T> results;
            results.reserve(this->size());
            for (std::future<T>& future : *this)
                results.push_back(future.get());
            return results;
        }
    }

    /**
     * @brief Check how many of the futures stored in this `multi_future` are ready.
     *
     * @return The number of ready futures.
     */
    [[nodiscard]] size_t ready_count() const
    {
        size_t count = 0;
        for (const std::future<T>& future : *this)
        {
            if (future.wait_for(std::chrono::duration<double>::zero()) == std::future_status::ready)
                ++count;
        }
        return count;
    }

    /**
     * @brief Check if all the futures stored in this `multi_future` are valid.
     *
     * @return `true` if all futures are valid, `false` if at least one of the futures is not valid.
     */
    [[nodiscard]] bool valid() const
    {
        bool is_valid = true;
        for (const std::future<T>& future : *this)
            is_valid = is_valid && future.valid();
        return is_valid;
    }

    /**
     * @brief Wait for all the futures stored in this `multi_future`.
     */
    void wait() const
    {
        for (const std::future<T>& future : *this)
            future.wait();
    }

    /**
     * @brief Wait for all the futures stored in this `multi_future`, but stop waiting after the specified duration has passed. This function first waits for the first future for the desired duration. If that future is ready before the duration expires, this function waits for the second future for whatever remains of the duration. It continues similarly until the duration expires.
     *
     * @tparam R An arithmetic type representing the number of ticks to wait.
     * @tparam P An `std::ratio` representing the length of each tick in seconds.
     * @param duration The amount of time to wait.
     * @return `true` if all futures have been waited for before the duration expired, `false` otherwise.
     */
    template <typename R, typename P>
    bool wait_for(const std::chrono::duration<R, P>& duration) const
    {
        const std::chrono::time_point<std::chrono::steady_clock> start_time = std::chrono::steady_clock::now();
        for (const std::future<T>& future : *this)
        {
            future.wait_for(duration - (std::chrono::steady_clock::now() - start_time));
            if (duration < std::chrono::steady_clock::now() - start_time)
                return false;
        }
        return true;
    }

    /**
     * @brief Wait for all the futures stored in this `multi_future`, but stop waiting after the specified time point has been reached. This function first waits for the first future until the desired time point. If that future is ready before the time point is reached, this function waits for the second future until the desired time point. It continues similarly until the time point is reached.
     *
     * @tparam C The type of the clock used to measure time.
     * @tparam D An `std::chrono::duration` type used to indicate the time point.
     * @param timeout_time The time point at which to stop waiting.
     * @return `true` if all futures have been waited for before the time point was reached, `false` otherwise.
     */
    template <typename C, typename D>
    bool wait_until(const std::chrono::time_point<C, D>& timeout_time) const
    {
        for (const std::future<T>& future : *this)
        {
            future.wait_until(timeout_time);
            if (timeout_time < std::chrono::steady_clock::now())
                return false;
        }
        return true;
    }
}; // class multi_future

/**
 * @brief A fast, lightweight, and easy-to-use C++17 thread pool class.
 */
class [[nodiscard]] thread_pool
{
public:
    // ============================
    // Constructors and destructors
    // ============================

    /**
     * @brief Construct a new thread pool. The number of threads will be the total number of hardware threads available, as reported by the implementation. This is usually determined by the number of cores in the CPU. If a core is hyperthreaded, it will count as two threads.
     */
    thread_pool() : thread_pool(0, [] {}) {}

    /**
     * @brief Construct a new thread pool with the specified number of threads.
     *
     * @param num_threads The number of threads to use.
     */
    explicit thread_pool(const concurrency_t num_threads) : thread_pool(num_threads, [] {}) {}

    /**
     * @brief Construct a new thread pool with the specified initialization function.
     *
     * @param init_task An initialization function to run in each thread before it starts to execute any submitted tasks. The function must take no arguments and have no return value. It will only be executed exactly once, when the thread is first constructed.
     */
    explicit thread_pool(const std::function<void()>& init_task) : thread_pool(0, init_task) {}

    /**
     * @brief Construct a new thread pool with the specified number of threads and initialization function.
     *
     * @param num_threads The number of threads to use.
     * @param init_task An initialization function to run in each thread before it starts to execute any submitted tasks. The function must take no arguments and have no return value. It will only be executed exactly once, when the thread is first constructed.
     */
    thread_pool(const concurrency_t num_threads, const std::function<void()>& init_task) : thread_count(determine_thread_count(num_threads)), threads(std::make_unique<std::thread[]>(determine_thread_count(num_threads)))
    {
        create_threads(init_task);
    }

    // The copy and move constructors and assignment operators are deleted. The thread pool uses a mutex, which cannot be copied or moved.
    thread_pool(const thread_pool&) = delete;
    thread_pool(thread_pool&&) = delete;
    thread_pool& operator=(const thread_pool&) = delete;
    thread_pool& operator=(thread_pool&&) = delete;

    /**
     * @brief Destruct the thread pool. Waits for all tasks to complete, then destroys all threads. Note that if the pool is paused, then any tasks still in the queue will never be executed.
     */
    ~thread_pool()
    {
        wait();
        destroy_threads();
    }

    // =======================
    // Public member functions
    // =======================

#ifdef BS_THREAD_POOL_ENABLE_NATIVE_HANDLES
    /**
     * @brief Get a vector containing the underlying implementation-defined thread handles for each of the pool's threads, as obtained by `std::thread::native_handle()`. Only enabled if `BS_THREAD_POOL_ENABLE_NATIVE_HANDLES` is defined.
     *
     * @return The native thread handles.
     */
    [[nodiscard]] std::vector<std::thread::native_handle_type> get_native_handles() const
    {
        std::vector<std::thread::native_handle_type> native_handles(thread_count);
        for (concurrency_t i = 0; i < thread_count; ++i)
        {
            native_handles[i] = threads[i].native_handle();
        }
        return native_handles;
    }
#endif

    /**
     * @brief Get the number of tasks currently waiting in the queue to be executed by the threads.
     *
     * @return The number of queued tasks.
     */
    [[nodiscard]] size_t get_tasks_queued() const
    {
        const std::scoped_lock tasks_lock(tasks_mutex);
        return tasks.size();
    }

    /**
     * @brief Get the number of tasks currently being executed by the threads.
     *
     * @return The number of running tasks.
     */
    [[nodiscard]] size_t get_tasks_running() const
    {
        const std::scoped_lock tasks_lock(tasks_mutex);
        return tasks_running;
    }

    /**
     * @brief Get the total number of unfinished tasks: either still waiting in the queue, or running in a thread. Note that `get_tasks_total() == get_tasks_queued() + get_tasks_running()`.
     *
     * @return The total number of tasks.
     */
    [[nodiscard]] size_t get_tasks_total() const
    {
        const std::scoped_lock tasks_lock(tasks_mutex);
        return tasks_running + tasks.size();
    }

    /**
     * @brief Get the number of threads in the pool.
     *
     * @return The number of threads.
     */
    [[nodiscard]] concurrency_t get_thread_count() const
    {
        return thread_count;
    }

    /**
     * @brief Get a vector containing the unique identifiers for each of the pool's threads, as obtained by `std::thread::get_id()`.
     *
     * @return The unique thread identifiers.
     */
    [[nodiscard]] std::vector<std::thread::id> get_thread_ids() const
    {
        std::vector<std::thread::id> thread_ids(thread_count);
        for (concurrency_t i = 0; i < thread_count; ++i)
        {
            thread_ids[i] = threads[i].get_id();
        }
        return thread_ids;
    }

#ifdef BS_THREAD_POOL_ENABLE_PAUSE
    /**
     * @brief Check whether the pool is currently paused. Only enabled if `BS_THREAD_POOL_ENABLE_PAUSE` is defined.
     *
     * @return `true` if the pool is paused, `false` if it is not paused.
     */
    [[nodiscard]] bool is_paused() const
    {
        const std::scoped_lock tasks_lock(tasks_mutex);
        return paused;
    }

    /**
     * @brief Pause the pool. The workers will temporarily stop retrieving new tasks out of the queue, although any tasks already executed will keep running until they are finished. Only enabled if `BS_THREAD_POOL_ENABLE_PAUSE` is defined.
     */
    void pause()
    {
        const std::scoped_lock tasks_lock(tasks_mutex);
        paused = true;
    }
#endif

    /**
     * @brief Purge all the tasks waiting in the queue. Tasks that are currently running will not be affected, but any tasks still waiting in the queue will be discarded, and will never be executed by the threads. Please note that there is no way to restore the purged tasks.
     */
    void purge()
    {
        const std::scoped_lock tasks_lock(tasks_mutex);
        while (!tasks.empty())
            tasks.pop();
    }

    /**
     * @brief Submit a function with no arguments and no return value into the task queue, with the specified priority. To push a function with arguments, enclose it in a lambda expression. Does not return a future, so the user must use `wait()` or some other method to ensure that the task finishes executing, otherwise bad things will happen.
     *
     * @tparam F The type of the function.
     * @param task The function to push.
     * @param priority The priority of the task. Should be between -32,768 and 32,767 (a signed 16-bit integer). The default is 0. Only enabled if `BS_THREAD_POOL_ENABLE_PRIORITY` is defined.
     */
    template <typename F>
    void detach_task(F&& task BS_THREAD_POOL_PRIORITY_INPUT)
    {
        {
            const std::scoped_lock tasks_lock(tasks_mutex);
            tasks.emplace(std::forward<F>(task) BS_THREAD_POOL_PRIORITY_OUTPUT);
        }
        task_available_cv.notify_one();
    }

    /**
     * @brief Parallelize a loop by automatically splitting it into blocks and submitting each block separately to the queue, with the specified priority. The block function takes two arguments, the start and end of the block, so that it is only called only once per block, but it is up to the user make sure the block function correctly deals with all the indices in each block. Does not return a `multi_future`, so the user must use `wait()` or some other method to ensure that the loop finishes executing, otherwise bad things will happen.
     *
     * @tparam T The type of the indices. Should be a signed or unsigned integer.
     * @tparam F The type of the function to loop through.
     * @param first_index The first index in the loop.
     * @param index_after_last The index after the last index in the loop. The loop will iterate from `first_index` to `(index_after_last - 1)` inclusive. In other words, it will be equivalent to `for (T i = first_index; i < index_after_last; ++i)`. Note that if `index_after_last <= first_index`, no blocks will be submitted.
     * @param block A function that will be called once per block. Should take exactly two arguments: the first index in the block and the index after the last index in the block. `block(start, end)` should typically involve a loop of the form `for (T i = start; i < end; ++i)`.
     * @param num_blocks The maximum number of blocks to split the loop into. The default is 0, which means the number of blocks will be equal to the number of threads in the pool.
     * @param priority The priority of the tasks. Should be between -32,768 and 32,767 (a signed 16-bit integer). The default is 0. Only enabled if `BS_THREAD_POOL_ENABLE_PRIORITY` is defined.
     */
    template <typename T, typename F>
    void detach_blocks(const T first_index, const T index_after_last, F&& block, const size_t num_blocks = 0 BS_THREAD_POOL_PRIORITY_INPUT)
    {
        if (index_after_last > first_index)
        {
            const blocks blks(first_index, index_after_last, num_blocks ? num_blocks : thread_count);
            for (size_t blk = 0; blk < blks.get_num_blocks(); ++blk)
                detach_task(
                    [block = std::forward<F>(block), start = blks.start(blk), end = blks.end(blk)]
                    {
                        block(start, end);
                    } BS_THREAD_POOL_PRIORITY_OUTPUT);
        }
    }

    /**
     * @brief Parallelize a loop by automatically splitting it into blocks and submitting each block separately to the queue, with the specified priority. The loop function takes one argument, the loop index, so that it is called many times per block. Does not return a `multi_future`, so the user must use `wait()` or some other method to ensure that the loop finishes executing, otherwise bad things will happen.
     *
     * @tparam T The type of the indices. Should be a signed or unsigned integer.
     * @tparam F The type of the function to loop through.
     * @param first_index The first index in the loop.
     * @param index_after_last The index after the last index in the loop. The loop will iterate from `first_index` to `(index_after_last - 1)` inclusive. In other words, it will be equivalent to `for (T i = first_index; i < index_after_last; ++i)`. Note that if `index_after_last <= first_index`, no blocks will be submitted.
     * @param loop The function to loop through. Will be called once per index, many times per block. Should take exactly one argument: the loop index.
     * @param num_blocks The maximum number of blocks to split the loop into. The default is 0, which means the number of blocks will be equal to the number of threads in the pool.
     * @param priority The priority of the tasks. Should be between -32,768 and 32,767 (a signed 16-bit integer). The default is 0. Only enabled if `BS_THREAD_POOL_ENABLE_PRIORITY` is defined.
     */
    template <typename T, typename F>
    void detach_loop(const T first_index, const T index_after_last, F&& loop, const size_t num_blocks = 0 BS_THREAD_POOL_PRIORITY_INPUT)
    {
        if (index_after_last > first_index)
        {
            const blocks blks(first_index, index_after_last, num_blocks ? num_blocks : thread_count);
            for (size_t blk = 0; blk < blks.get_num_blocks(); ++blk)
                detach_task(
                    [loop = std::forward<F>(loop), start = blks.start(blk), end = blks.end(blk)]
                    {
                        for (T i = start; i < end; ++i)
                            loop(i);
                    } BS_THREAD_POOL_PRIORITY_OUTPUT);
        }
    }

    /**
     * @brief Submit a sequence of tasks enumerated by indices to the queue, with the specified priority. Does not return a `multi_future`, so the user must use `wait()` or some other method to ensure that the sequence finishes executing, otherwise bad things will happen.
     *
     * @tparam T The type of the indices. Should be a signed or unsigned integer.
     * @tparam F The type of the function used to define the sequence.
     * @param first_index The first index in the sequence.
     * @param index_after_last The index after the last index in the sequence. The sequence will iterate from `first_index` to `(index_after_last - 1)` inclusive. In other words, it will be equivalent to `for (T i = first_index; i < index_after_last; ++i)`. Note that if `index_after_last <= first_index`, no tasks will be submitted.
     * @param sequence The function used to define the sequence. Will be called once per index. Should take exactly one argument, the index.
     * @param priority The priority of the tasks. Should be between -32,768 and 32,767 (a signed 16-bit integer). The default is 0. Only enabled if `BS_THREAD_POOL_ENABLE_PRIORITY` is defined.
     */
    template <typename T, typename F>
    void detach_sequence(const T first_index, const T index_after_last, F&& sequence BS_THREAD_POOL_PRIORITY_INPUT)
    {
        for (T i = first_index; i < index_after_last; ++i)
            detach_task(
                [sequence = std::forward<F>(sequence), i]
                {
                    sequence(i);
                } BS_THREAD_POOL_PRIORITY_OUTPUT);
    }

    /**
     * @brief Reset the pool with the total number of hardware threads available, as reported by the implementation. Waits for all currently running tasks to be completed, then destroys all threads in the pool and creates a new thread pool with the new number of threads. Any tasks that were waiting in the queue before the pool was reset will then be executed by the new threads. If the pool was paused before resetting it, the new pool will be paused as well.
     */
    void reset()
    {
        reset(0, [] {});
    }

    /**
     * @brief Reset the pool with a new number of threads. Waits for all currently running tasks to be completed, then destroys all threads in the pool and creates a new thread pool with the new number of threads. Any tasks that were waiting in the queue before the pool was reset will then be executed by the new threads. If the pool was paused before resetting it, the new pool will be paused as well.
     *
     * @param num_threads The number of threads to use.
     */
    void reset(const concurrency_t num_threads)
    {
        reset(num_threads, [] {});
    }

    /**
     * @brief Reset the pool with the total number of hardware threads available, as reported by the implementation, and a new initialization function. Waits for all currently running tasks to be completed, then destroys all threads in the pool and creates a new thread pool with the new number of threads and initialization function. Any tasks that were waiting in the queue before the pool was reset will then be executed by the new threads. If the pool was paused before resetting it, the new pool will be paused as well.
     *
     * @param init_task An initialization function to run in each thread before it starts to execute any submitted tasks. The function must take no arguments and have no return value. It will only be executed exactly once, when the thread is first constructed.
     */
    void reset(const std::function<void()>& init_task)
    {
        reset(0, init_task);
    }

    /**
     * @brief Reset the pool with a new number of threads and a new initialization function. Waits for all currently running tasks to be completed, then destroys all threads in the pool and creates a new thread pool with the new number of threads and initialization function. Any tasks that were waiting in the queue before the pool was reset will then be executed by the new threads. If the pool was paused before resetting it, the new pool will be paused as well.
     *
     * @param num_threads The number of threads to use.
     * @param init_task An initialization function to run in each thread before it starts to execute any submitted tasks. The function must take no arguments and have no return value. It will only be executed exactly once, when the thread is first constructed.
     */
    void reset(const concurrency_t num_threads, const std::function<void()>& init_task)
    {
#ifdef BS_THREAD_POOL_ENABLE_PAUSE
        std::unique_lock tasks_lock(tasks_mutex);
        const bool was_paused = paused;
        paused = true;
        tasks_lock.unlock();
#endif
        wait();
        destroy_threads();
        thread_count = determine_thread_count(num_threads);
        threads = std::make_unique<std::thread[]>(thread_count);
        create_threads(init_task);
#ifdef BS_THREAD_POOL_ENABLE_PAUSE
        tasks_lock.lock();
        paused = was_paused;
#endif
    }

    /**
     * @brief Submit a function with no arguments into the task queue, with the specified priority. To submit a function with arguments, enclose it in a lambda expression. If the function has a return value, get a future for the eventual returned value. If the function has no return value, get an `std::future<void>` which can be used to wait until the task finishes.
     *
     * @tparam F The type of the function.
     * @tparam R The return type of the function (can be `void`).
     * @param task The function to submit.
     * @param priority The priority of the task. Should be between -32,768 and 32,767 (a signed 16-bit integer). The default is 0. Only enabled if `BS_THREAD_POOL_ENABLE_PRIORITY` is defined.
     * @return A future to be used later to wait for the function to finish executing and/or obtain its returned value if it has one.
     */
    template <typename F, typename R = std::invoke_result_t<std::decay_t<F>>>
    [[nodiscard]] std::future<R> submit_task(F&& task BS_THREAD_POOL_PRIORITY_INPUT)
    {
        const std::shared_ptr<std::promise<R>> task_promise = std::make_shared<std::promise<R>>();
        detach_task(
            [task = std::forward<F>(task), task_promise]
            {
#ifndef BS_THREAD_POOL_DISABLE_EXCEPTION_HANDLING
                try
                {
#endif
                    if constexpr (std::is_void_v<R>)
                    {
                        task();
                        task_promise->set_value();
                    }
                    else
                    {
                        task_promise->set_value(task());
                    }
#ifndef BS_THREAD_POOL_DISABLE_EXCEPTION_HANDLING
                }
                catch (...)
                {
                    try
                    {
                        task_promise->set_exception(std::current_exception());
                    }
                    catch (...)
                    {
                    }
                }
#endif
            } BS_THREAD_POOL_PRIORITY_OUTPUT);
        return task_promise->get_future();
    }

    /**
     * @brief Parallelize a loop by automatically splitting it into blocks and submitting each block separately to the queue, with the specified priority. The block function takes two arguments, the start and end of the block, so that it is only called only once per block, but it is up to the user make sure the block function correctly deals with all the indices in each block. Returns a `multi_future` that contains the futures for all of the blocks.
     *
     * @tparam T The type of the indices. Should be a signed or unsigned integer.
     * @tparam F The type of the function to loop through.
     * @tparam R The return type of the function to loop through (can be `void`).
     * @param first_index The first index in the loop.
     * @param index_after_last The index after the last index in the loop. The loop will iterate from `first_index` to `(index_after_last - 1)` inclusive. In other words, it will be equivalent to `for (T i = first_index; i < index_after_last; ++i)`. Note that if `index_after_last <= first_index`, no blocks will be submitted, and an empty `multi_future` will be returned.
     * @param block A function that will be called once per block. Should take exactly two arguments: the first index in the block and the index after the last index in the block. `block(start, end)` should typically involve a loop of the form `for (T i = start; i < end; ++i)`.
     * @param num_blocks The maximum number of blocks to split the loop into. The default is 0, which means the number of blocks will be equal to the number of threads in the pool.
     * @param priority The priority of the tasks. Should be between -32,768 and 32,767 (a signed 16-bit integer). The default is 0. Only enabled if `BS_THREAD_POOL_ENABLE_PRIORITY` is defined.
     * @return A `multi_future` that can be used to wait for all the blocks to finish. If the block function returns a value, the `multi_future` can also be used to obtain the values returned by each block.
     */
    template <typename T, typename F, typename R = std::invoke_result_t<std::decay_t<F>, T, T>>
    [[nodiscard]] multi_future<R> submit_blocks(const T first_index, const T index_after_last, F&& block, const size_t num_blocks = 0 BS_THREAD_POOL_PRIORITY_INPUT)
    {
        if (index_after_last > first_index)
        {
            const blocks blks(first_index, index_after_last, num_blocks ? num_blocks : thread_count);
            multi_future<R> future;
            future.reserve(blks.get_num_blocks());
            for (size_t blk = 0; blk < blks.get_num_blocks(); ++blk)
                future.push_back(submit_task(
                    [block = std::forward<F>(block), start = blks.start(blk), end = blks.end(blk)]
                    {
                        return block(start, end);
                    } BS_THREAD_POOL_PRIORITY_OUTPUT));
            return future;
        }
        return {};
    }

    /**
     * @brief Parallelize a loop by automatically splitting it into blocks and submitting each block separately to the queue, with the specified priority. The loop function takes one argument, the loop index, so that it is called many times per block. It must have no return value. Returns a `multi_future` that contains the futures for all of the blocks.
     *
     * @tparam T The type of the indices. Should be a signed or unsigned integer.
     * @tparam F The type of the function to loop through.
     * @param first_index The first index in the loop.
     * @param index_after_last The index after the last index in the loop. The loop will iterate from `first_index` to `(index_after_last - 1)` inclusive. In other words, it will be equivalent to `for (T i = first_index; i < index_after_last; ++i)`. Note that if `index_after_last <= first_index`, no tasks will be submitted, and an empty `multi_future` will be returned.
     * @param loop The function to loop through. Will be called once per index, many times per block. Should take exactly one argument: the loop index. It cannot have a return value.
     * @param num_blocks The maximum number of blocks to split the loop into. The default is 0, which means the number of blocks will be equal to the number of threads in the pool.
     * @param priority The priority of the tasks. Should be between -32,768 and 32,767 (a signed 16-bit integer). The default is 0. Only enabled if `BS_THREAD_POOL_ENABLE_PRIORITY` is defined.
     * @return A `multi_future` that can be used to wait for all the blocks to finish.
     */
    template <typename T, typename F>
    [[nodiscard]] multi_future<void> submit_loop(const T first_index, const T index_after_last, F&& loop, const size_t num_blocks = 0 BS_THREAD_POOL_PRIORITY_INPUT)
    {
        if (index_after_last > first_index)
        {
            const blocks blks(first_index, index_after_last, num_blocks ? num_blocks : thread_count);
            multi_future<void> future;
            future.reserve(blks.get_num_blocks());
            for (size_t blk = 0; blk < blks.get_num_blocks(); ++blk)
                future.push_back(submit_task(
                    [loop = std::forward<F>(loop), start = blks.start(blk), end = blks.end(blk)]
                    {
                        for (T i = start; i < end; ++i)
                            loop(i);
                    } BS_THREAD_POOL_PRIORITY_OUTPUT));
            return future;
        }
        return {};
    }

    /**
     * @brief Submit a sequence of tasks enumerated by indices to the queue, with the specified priority. Returns a `multi_future` that contains the futures for all of the tasks.
     *
     * @tparam T The type of the indices. Should be a signed or unsigned integer.
     * @tparam F The type of the function used to define the sequence.
     * @tparam R The return type of the function used to define the sequence (can be `void`).
     * @param first_index The first index in the sequence.
     * @param index_after_last The index after the last index in the sequence. The sequence will iterate from `first_index` to `(index_after_last - 1)` inclusive. In other words, it will be equivalent to `for (T i = first_index; i < index_after_last; ++i)`. Note that if `index_after_last <= first_index`, no tasks will be submitted, and an empty `multi_future` will be returned.
     * @param sequence The function used to define the sequence. Will be called once per index. Should take exactly one argument, the index.
     * @param priority The priority of the tasks. Should be between -32,768 and 32,767 (a signed 16-bit integer). The default is 0. Only enabled if `BS_THREAD_POOL_ENABLE_PRIORITY` is defined.
     * @return A `multi_future` that can be used to wait for all the tasks to finish. If the sequence function returns a value, the `multi_future` can also be used to obtain the values returned by each task.
     */
    template <typename T, typename F, typename R = std::invoke_result_t<std::decay_t<F>, T>>
    [[nodiscard]] multi_future<R> submit_sequence(const T first_index, const T index_after_last, F&& sequence BS_THREAD_POOL_PRIORITY_INPUT)
    {
        if (index_after_last > first_index)
        {
            multi_future<R> future;
            future.reserve(static_cast<size_t>(index_after_last - first_index));
            for (T i = first_index; i < index_after_last; ++i)
                future.push_back(submit_task(
                    [sequence = std::forward<F>(sequence), i]
                    {
                        return sequence(i);
                    } BS_THREAD_POOL_PRIORITY_OUTPUT));
            return future;
        }
        return {};
    }

#ifdef BS_THREAD_POOL_ENABLE_PAUSE
    /**
     * @brief Unpause the pool. The workers will resume retrieving new tasks out of the queue. Only enabled if `BS_THREAD_POOL_ENABLE_PAUSE` is defined.
     */
    void unpause()
    {
        {
            const std::scoped_lock tasks_lock(tasks_mutex);
            paused = false;
        }
        task_available_cv.notify_all();
    }
#endif

// Macros used internally to enable or disable pausing in the waiting and worker functions.
#ifdef BS_THREAD_POOL_ENABLE_PAUSE
    #define BS_THREAD_POOL_PAUSED_OR_EMPTY (paused || tasks.empty())
#else
    #define BS_THREAD_POOL_PAUSED_OR_EMPTY tasks.empty()
#endif

    /**
     * @brief Wait for tasks to be completed. Normally, this function waits for all tasks, both those that are currently running in the threads and those that are still waiting in the queue. However, if the pool is paused, this function only waits for the currently running tasks (otherwise it would wait forever). Note: To wait for just one specific task, use `submit_task()` instead, and call the `wait()` member function of the generated future.
     *
     * @throws `wait_deadlock` if called from within a thread of the same pool, which would result in a deadlock. Only enabled if `BS_THREAD_POOL_ENABLE_WAIT_DEADLOCK_CHECK` is defined.
     */
    void wait()
    {
#ifdef BS_THREAD_POOL_ENABLE_WAIT_DEADLOCK_CHECK
        if (this_thread::get_pool() == this)
            throw wait_deadlock();
#endif
        std::unique_lock tasks_lock(tasks_mutex);
        waiting = true;
        tasks_done_cv.wait(tasks_lock,
            [this]
            {
                return (tasks_running == 0) && BS_THREAD_POOL_PAUSED_OR_EMPTY;
            });
        waiting = false;
    }

    /**
     * @brief Wait for tasks to be completed, but stop waiting after the specified duration has passed.
     *
     * @tparam R An arithmetic type representing the number of ticks to wait.
     * @tparam P An `std::ratio` representing the length of each tick in seconds.
     * @param duration The amount of time to wait.
     * @return `true` if all tasks finished running, `false` if the duration expired but some tasks are still running.
     *
     * @throws `wait_deadlock` if called from within a thread of the same pool, which would result in a deadlock. Only enabled if `BS_THREAD_POOL_ENABLE_WAIT_DEADLOCK_CHECK` is defined.
     */
    template <typename R, typename P>
    bool wait_for(const std::chrono::duration<R, P>& duration)
    {
#ifdef BS_THREAD_POOL_ENABLE_WAIT_DEADLOCK_CHECK
        if (this_thread::get_pool() == this)
            throw wait_deadlock();
#endif
        std::unique_lock tasks_lock(tasks_mutex);
        waiting = true;
        const bool status = tasks_done_cv.wait_for(tasks_lock, duration,
            [this]
            {
                return (tasks_running == 0) && BS_THREAD_POOL_PAUSED_OR_EMPTY;
            });
        waiting = false;
        return status;
    }

    /**
     * @brief Wait for tasks to be completed, but stop waiting after the specified time point has been reached.
     *
     * @tparam C The type of the clock used to measure time.
     * @tparam D An `std::chrono::duration` type used to indicate the time point.
     * @param timeout_time The time point at which to stop waiting.
     * @return `true` if all tasks finished running, `false` if the time point was reached but some tasks are still running.
     *
     * @throws `wait_deadlock` if called from within a thread of the same pool, which would result in a deadlock. Only enabled if `BS_THREAD_POOL_ENABLE_WAIT_DEADLOCK_CHECK` is defined.
     */
    template <typename C, typename D>
    bool wait_until(const std::chrono::time_point<C, D>& timeout_time)
    {
#ifdef BS_THREAD_POOL_ENABLE_WAIT_DEADLOCK_CHECK
        if (this_thread::get_pool() == this)
            throw wait_deadlock();
#endif
        std::unique_lock tasks_lock(tasks_mutex);
        waiting = true;
        const bool status = tasks_done_cv.wait_until(tasks_lock, timeout_time,
            [this]
            {
                return (tasks_running == 0) && BS_THREAD_POOL_PAUSED_OR_EMPTY;
            });
        waiting = false;
        return status;
    }

#ifdef BS_THREAD_POOL_ENABLE_WAIT_DEADLOCK_CHECK
    // ==============
    // Public classes
    // ==============

    /**
     * @brief An exception that will be thrown by `wait()`, `wait_for()`, and `wait_until()` if the user tries to call them from within a thread of the same pool, which would result in a deadlock.
     */
    struct wait_deadlock : public std::runtime_error
    {
        wait_deadlock() : std::runtime_error("BS::thread_pool::wait_deadlock"){};
    };
#endif

private:
    // ========================
    // Private member functions
    // ========================

    /**
     * @brief Create the threads in the pool and assign a worker to each thread.
     *
     * @param init_task An initialization function to run in each thread before it starts to execute any submitted tasks.
     */
    void create_threads(const std::function<void()>& init_task)
    {
        {
            const std::scoped_lock tasks_lock(tasks_mutex);
            tasks_running = thread_count;
            workers_running = true;
        }
        for (concurrency_t i = 0; i < thread_count; ++i)
        {
            threads[i] = std::thread(&thread_pool::worker, this, i, init_task);
        }
    }

    /**
     * @brief Destroy the threads in the pool.
     */
    void destroy_threads()
    {
        {
            const std::scoped_lock tasks_lock(tasks_mutex);
            workers_running = false;
        }
        task_available_cv.notify_all();
        for (concurrency_t i = 0; i < thread_count; ++i)
        {
            threads[i].join();
        }
    }

    /**
     * @brief Determine how many threads the pool should have, based on the parameter passed to the constructor or reset().
     *
     * @param num_threads The parameter passed to the constructor or `reset()`. If the parameter is a positive number, then the pool will be created with this number of threads. If the parameter is non-positive, or a parameter was not supplied (in which case it will have the default value of 0), then the pool will be created with the total number of hardware threads available, as obtained from `std::thread::hardware_concurrency()`. If the latter returns zero for some reason, then the pool will be created with just one thread.
     * @return The number of threads to use for constructing the pool.
     */
    [[nodiscard]] static concurrency_t determine_thread_count(const concurrency_t num_threads)
    {
        if (num_threads > 0)
            return num_threads;
        if (std::thread::hardware_concurrency() > 0)
            return std::thread::hardware_concurrency();
        return 1;
    }

    /**
     * @brief A worker function to be assigned to each thread in the pool. Waits until it is notified by `detach_task()` that a task is available, and then retrieves the task from the queue and executes it. Once the task finishes, the worker notifies `wait()` in case it is waiting.
     *
     * @param idx The index of this thread.
     * @param init_task An initialization function to run in this thread before it starts to execute any submitted tasks.
     */
    void worker(const concurrency_t idx, const std::function<void()>& init_task)
    {
        this_thread::get_index.index = idx;
        this_thread::get_pool.pool = this;
        init_task();
        std::unique_lock tasks_lock(tasks_mutex);
        while (true)
        {
            --tasks_running;
            tasks_lock.unlock();
            if (waiting && (tasks_running == 0) && BS_THREAD_POOL_PAUSED_OR_EMPTY)
                tasks_done_cv.notify_all();
            tasks_lock.lock();
            task_available_cv.wait(tasks_lock,
                [this]
                {
                    return !BS_THREAD_POOL_PAUSED_OR_EMPTY || !workers_running;
                });
            if (!workers_running)
                break;
            {
#ifdef BS_THREAD_POOL_ENABLE_PRIORITY
                const std::function<void()> task = std::move(std::remove_const_t<pr_task&>(tasks.top()).task);
                tasks.pop();
#else
                const std::function<void()> task = std::move(tasks.front());
                tasks.pop();
#endif
                ++tasks_running;
                tasks_lock.unlock();
                task();
            }
            tasks_lock.lock();
        }
        this_thread::get_index.index = std::nullopt;
        this_thread::get_pool.pool = std::nullopt;
    }

    // ===============
    // Private classes
    // ===============

    /**
     * @brief A helper class to divide a range into blocks. Used by `detach_blocks()`, `submit_blocks()`, `detach_loop()`, and `submit_loop()`.
     *
     * @tparam T The type of the indices. Should be a signed or unsigned integer.
     */
    template <typename T>
    class [[nodiscard]] blocks
    {
    public:
        /**
         * @brief Construct a `blocks` object with the given specifications.
         *
         * @param first_index_ The first index in the range.
         * @param index_after_last_ The index after the last index in the range.
         * @param num_blocks_ The desired number of blocks to divide the range into.
         */
        blocks(const T first_index_, const T index_after_last_, const size_t num_blocks_) : first_index(first_index_), index_after_last(index_after_last_), num_blocks(num_blocks_)
        {
            if (index_after_last > first_index)
            {
                const size_t total_size = static_cast<size_t>(index_after_last - first_index);
                if (num_blocks > total_size)
                    num_blocks = total_size;
                block_size = total_size / num_blocks;
                remainder = total_size % num_blocks;
                if (block_size == 0)
                {
                    block_size = 1;
                    num_blocks = (total_size > 1) ? total_size : 1;
                }
            }
            else
            {
                num_blocks = 0;
            }
        }

        /**
         * @brief Get the first index of a block.
         *
         * @param block The block number.
         * @return The first index.
         */
        [[nodiscard]] T start(const size_t block) const
        {
            return first_index + static_cast<T>(block * block_size) + static_cast<T>(block < remainder ? block : remainder);
        }

        /**
         * @brief Get the index after the last index of a block.
         *
         * @param block The block number.
         * @return The index after the last index.
         */
        [[nodiscard]] T end(const size_t block) const
        {
            return (block == num_blocks - 1) ? index_after_last : start(block + 1);
        }

        /**
         * @brief Get the number of blocks. Note that this may be different than the desired number of blocks that was passed to the constructor.
         *
         * @return The number of blocks.
         */
        [[nodiscard]] size_t get_num_blocks() const
        {
            return num_blocks;
        }

    private:
        /**
         * @brief The size of each block (except possibly the last block).
         */
        size_t block_size = 0;

        /**
         * @brief The first index in the range.
         */
        T first_index = 0;

        /**
         * @brief The index after the last index in the range.
         */
        T index_after_last = 0;

        /**
         * @brief The number of blocks.
         */
        size_t num_blocks = 0;

        /**
         * @brief The remainder obtained after dividing the total size by the number of blocks.
         */
        size_t remainder = 0;
    }; // class blocks

#ifdef BS_THREAD_POOL_ENABLE_PRIORITY
    /**
     * @brief A helper class to store a task with an assigned priority.
     */
    class [[nodiscard]] pr_task
    {
        friend class thread_pool;

    public:
        /**
         * @brief Construct a new task with an assigned priority by copying the task.
         *
         * @param task_ The task.
         * @param priority_ The desired priority.
         */
        explicit pr_task(const std::function<void()>& task_, const priority_t priority_ = 0) : task(task_), priority(priority_) {}

        /**
         * @brief Construct a new task with an assigned priority by moving the task.
         *
         * @param task_ The task.
         * @param priority_ The desired priority.
         */
        explicit pr_task(std::function<void()>&& task_, const priority_t priority_ = 0) : task(std::move(task_)), priority(priority_) {}

        /**
         * @brief Compare the priority of two tasks.
         *
         * @param lhs The first task.
         * @param rhs The second task.
         * @return `true` if the first task has a lower priority than the second task, `false` otherwise.
         */
        [[nodiscard]] friend bool operator<(const pr_task& lhs, const pr_task& rhs)
        {
            return lhs.priority < rhs.priority;
        }

    private:
        /**
         * @brief The task.
         */
        std::function<void()> task = {};

        /**
         * @brief The priority of the task.
         */
        priority_t priority = 0;
    }; // class pr_task
#endif

    // ============
    // Private data
    // ============

#ifdef BS_THREAD_POOL_ENABLE_PAUSE
    /**
     * @brief A flag indicating whether the workers should pause. When set to `true`, the workers temporarily stop retrieving new tasks out of the queue, although any tasks already executed will keep running until they are finished. When set to `false` again, the workers resume retrieving tasks.
     */
    bool paused = false;
#endif

    /**
     * @brief A condition variable to notify `worker()` that a new task has become available.
     */
    std::condition_variable task_available_cv = {};

    /**
     * @brief A condition variable to notify `wait()` that the tasks are done.
     */
    std::condition_variable tasks_done_cv = {};

    /**
     * @brief A queue of tasks to be executed by the threads.
     */
#ifdef BS_THREAD_POOL_ENABLE_PRIORITY
    std::priority_queue<pr_task> tasks = {};
#else
    std::queue<std::function<void()>> tasks = {};
#endif

    /**
     * @brief A counter for the total number of currently running tasks.
     */
    size_t tasks_running = 0;

    /**
     * @brief A mutex to synchronize access to the task queue by different threads.
     */
    mutable std::mutex tasks_mutex = {};

    /**
     * @brief The number of threads in the pool.
     */
    concurrency_t thread_count = 0;

    /**
     * @brief A smart pointer to manage the memory allocated for the threads.
     */
    std::unique_ptr<std::thread[]> threads = nullptr;

    /**
     * @brief A flag indicating that `wait()` is active and expects to be notified whenever a task is done.
     */
    bool waiting = false;

    /**
     * @brief A flag indicating to the workers to keep running. When set to `false`, the workers terminate permanently.
     */
    bool workers_running = false;
}; // class thread_pool
} // namespace BS

// ======================================================================
//  BSThreadPoolExecutor - uniflow IExecutor adapter over BS::thread_pool.
//  Defined here so the complete BS::thread_pool type is visible as a member.
// ======================================================================
namespace uniflow
{

class BSThreadPoolExecutor : public IExecutor
{
public:
    explicit BSThreadPoolExecutor(std::size_t threads = 0)
        : pool_(threads ? threads : std::thread::hardware_concurrency()) {}

    void Submit(std::function<void()> task) override
    {
        pool_.detach_task(std::move(task));
    }
    size_t QueueDepth() const override { return pool_.get_tasks_queued(); }

private:
    BS::thread_pool pool_;
};

} // namespace uniflow
