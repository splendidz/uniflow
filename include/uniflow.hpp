// ============================================================================
//  uniflow.hpp — single-threaded, step-driven cooperative async framework
//
//  A module expresses its logic as a chain of step functions. One pump thread
//  per runtime drives every module round-robin; blocking work is offloaded to
//  a thread pool and its result is delivered to the next step. The runtime is
//  hidden — you never hold it. See DESIGN.md for the rationale.
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
//          OrderRouter::inst().Start(&OrderRouter::OnRoute_Begin, Message{...});
//          OrderRouter::inst().Wait();
//      }
//
//  Single header, C++17.
// ============================================================================
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

// BS::thread_pool — bshoshany/thread-pool. Included only when the bundled
// adapter is needed, so the framework stays usable with any other pool.
#if defined(UNIFLOW_USE_BS_THREAD_POOL)
#include "BS_thread_pool.hpp"
#endif

namespace uniflow
{

// ─────────────────────────────────────────────────────────────────────────────
//  Time
// ─────────────────────────────────────────────────────────────────────────────
using Clock     = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration  = Clock::duration;

inline double to_ms(Duration d)
{
    return std::chrono::duration<double, std::milli>(d).count();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Step result vocabulary — a step returns an *intent*, not a state change.
// ─────────────────────────────────────────────────────────────────────────────
enum class StepAction : uint8_t
{
    Stay,    // re-run the same step next tick (busy cooperative poll)
    Wait,    // re-run the same step, but only after a delay (paced poll)
    Advance, // move to result.next_fn next tick
    Done,    // flow completed normally → idle
    Fail     // flow aborted → idle
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

// ─────────────────────────────────────────────────────────────────────────────
//  Async support types
// ─────────────────────────────────────────────────────────────────────────────

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
// `_val` points into the owning module's slot — use it before the step
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

// ─────────────────────────────────────────────────────────────────────────────
//  Executor abstraction (whatever thread pool you bring)
// ─────────────────────────────────────────────────────────────────────────────
class IExecutor
{
public:
    virtual ~IExecutor()                              = default;
    virtual void   Submit(std::function<void()> task) = 0;
    virtual size_t QueueDepth() const                 = 0;
};

// Runs the task synchronously on the calling thread. Useful for tests — the
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

#if defined(UNIFLOW_USE_BS_THREAD_POOL)
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
#endif

// ─────────────────────────────────────────────────────────────────────────────
//  Trace + observer
// ─────────────────────────────────────────────────────────────────────────────
// One recorded event in a flow's trace. The trace is the ordered list of
// everything that happened during a flow; OnFlowEnded receives the whole
// vector — that is what makes "how far did it get before failing" answerable.
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
// these hooks — the framework itself never touches std::cout. Subclass and
// install with uniflow::SetObserver; override only the events you care about.
class IUniflowObserver
{
public:
    virtual ~IUniflowObserver() = default;

    virtual void OnFlowStarted(std::string_view /*obj*/) {}
    virtual void OnStepRan(std::string_view /*obj*/, std::string_view /*step*/,
                           int /*ordinal*/, Duration /*cpu*/) {}
    virtual void OnAsyncSubmitted(std::string_view /*obj*/, std::string_view /*step*/,
                                  std::string_view /*job*/) {}
    virtual void OnAsyncCompleted(std::string_view /*obj*/, std::string_view /*job*/,
                                  Duration /*wait*/, bool /*had_error*/,
                                  bool /*timed_out*/) {}
    virtual void OnSlowCpuStep(std::string_view /*obj*/, std::string_view /*step*/,
                               Duration /*cpu*/) {}
    virtual void OnSlowAsync(std::string_view /*obj*/, std::string_view /*job*/,
                             Duration /*wait_so_far*/) {}
    virtual void OnFlowEnded(std::string_view /*obj*/, StepAction /*terminal_action*/,
                             int /*reached_ordinal*/,
                             const std::vector<TraceEntry>& /*trace*/,
                             Duration /*wall_clock*/, Duration /*total_cpu*/,
                             Duration /*total_async_wait*/,
                             const FlowStats& /*stats*/) {}
};

// Default observer — pretty-prints to stdout. Thread-safe: every runtime's
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

// ─────────────────────────────────────────────────────────────────────────────
//  Config — global, set once before the first module/flow via uniflow::Configure
// ─────────────────────────────────────────────────────────────────────────────
struct Config
{
    // A step holding the pump thread longer than this trips OnSlowCpuStep —
    // the single-thread-protection alarm.
    Duration slow_cpu_threshold = std::chrono::milliseconds(1);
    // How long a pump sleeps when every module on it is idle (no busy-spin).
    Duration idle_sleep = std::chrono::milliseconds(1);
};

// ─────────────────────────────────────────────────────────────────────────────
//  detail — the hidden machinery: runtime, pump threads, the static hub
// ─────────────────────────────────────────────────────────────────────────────
namespace detail
{

// Set once per pump thread to the index of the runtime it drives; -1 on any
// other thread. GetInst()/inst() use it to assert that cross-module access
// stays within one runtime (one pump thread) — see DESIGN.md §5-10.
inline thread_local int t_runtime_idx = -1;

// Uniflow<Derived> is a template, so a runtime cannot hold "all modules"
// directly — each module is a different type. IUniflowObject is the
// non-template base they share, so the pump can drive them uniformly.
class IUniflowObject
{
public:
    virtual ~IUniflowObject()                   = default;
    virtual bool IsIdle() const                 = 0; // no flow running?
    virtual bool ExecuteOnce(IUniflowObserver&) = 0; // tick; ran a step?
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
        if (pump_.joinable())
            pump_.join();
    }
    UniflowRuntime(const UniflowRuntime&)            = delete;
    UniflowRuntime& operator=(const UniflowRuntime&) = delete;

    int index() const { return index_; }

    // Register / unregister a module with this runtime's pump. A module's base
    // ctor attaches; its base dtor detaches. The pump holds objects_mu_ for the
    // whole round, so detach (from a non-pump thread) lands strictly between
    // rounds — the pump never touches a module mid-destruction.
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

private:
    void pump_loop(); // defined after Hub (it reads Hub's observer + config)

    int                          index_;
    std::recursive_mutex         objects_mu_; // recursive: a step may attach a
    std::vector<IUniflowObject*> objects_;    // freshly lazy-created module
    std::atomic<bool>            stop_{false};
    std::thread                  pump_;
};

// The static hub: owns the runtimes, the executors, the observer, the config.
// Lazily creates 1 runtime if uniflow::Init() was never called. A Meyers
// singleton — and the only one in the framework.
class Hub
{
public:
    static Hub& get()
    {
        static Hub h;
        return h;
    }

    // ── opt-in setup; all must run before the first module/flow exists ──
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

    // ── used by modules at construction / async submission ──
    UniflowRuntime& runtime(int idx)
    {
        start();
        if (idx < 0 || idx >= static_cast<int>(runtimes_.size()))
            throw std::out_of_range(
                "uniflow: runtime index " + std::to_string(idx)
                + " out of range [0," + std::to_string(runtimes_.size()) + ")");
        return *runtimes_[static_cast<std::size_t>(idx)];
    }
    int runtime_count()
    {
        start();
        return static_cast<int>(runtimes_.size());
    }
    IExecutor& executor(const std::string& name)
    {
        start();
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
        // Destroy the runtimes first — stops and joins every pump thread —
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
    void start()
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
    // Declared last → destroyed first: pumps join before observer_/cfg_ die.
    std::vector<std::unique_ptr<UniflowRuntime>>          runtimes_;
};

// The pump loop — the single thread that drives one runtime. Each round ticks
// every non-idle module once (round-robin: no module starves another). If the
// round did real work, loop again at once; if nothing was active, sleep one
// idle interval so an idle runtime costs no CPU.
inline void UniflowRuntime::pump_loop()
{
    t_runtime_idx = index_;
    for (;;)
    {
        if (stop_.load(std::memory_order_relaxed))
            return;
        bool ran_step = false;
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
                if (o->ExecuteOnce(Hub::get().observer()))
                    ran_step = true;
            }
        }
        // No module ran a step this round — every active one is gated on a
        // pending async or a Sleep(). Sleep rather than spin; async results
        // and Sleep() delays are millisecond-scale, so a 1ms poll suffices.
        if (!ran_step)
            std::this_thread::sleep_for(Hub::get().config().idle_sleep);
    }
}

} // namespace detail

// ─────────────────────────────────────────────────────────────────────────────
//  Public setup surface — all optional, all before the first module/flow
// ─────────────────────────────────────────────────────────────────────────────

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

// ─────────────────────────────────────────────────────────────────────────────
//  Uniflow<Derived> — the CRTP base your modules inherit
//
//  CRTP = Curiously Recurring Template Pattern: a module inherits from the base
//  parameterised on *itself* — `class Arm : Uniflow<Arm>`. That hands the base
//  the derived type at compile time, so step functions are pointers-to-member
//  of the derived class and run with no virtual dispatch.
//
//  Put UF_USES_UNIFLOW(Cls) — or UF_SINGLETON(Cls) — once in the class body.
// ─────────────────────────────────────────────────────────────────────────────
template <class Derived>
class Uniflow : public detail::IUniflowObject
{
public:
    // StepResult appears inside StepFn's type, so forward-declare it, define
    // StepFn, then define StepResult.
    struct StepResult;

    // A *steady* step is a member function of Derived taking no args, returning
    // a StepResult. The runtime stores one in curr_fn_ and calls it each tick.
    // (The entry step may take args — see Start — and is held as a thunk.)
    using StepFn = StepResult (Derived::*)();

    // What a step returns: an intent, not a state change. The runtime reads
    // `action` and, for Advance, follows next_fn. next_name is the step's name
    // string, captured by UF_NEXT(#fn) purely for the trace/log.
    struct StepResult
    {
        StepAction  action    = StepAction::Stay;
        StepFn      next_fn   = nullptr; // set only when action == Advance
        const char* next_name = nullptr; // step name for the trace/log
        Duration    delay     = {};      // set only when action == Wait
    };

    // ── Constructors (inherited into Derived via UF_USES_UNIFLOW) ──
    //   Uniflow()                       anonymous, runtime 0
    //   Uniflow("name")                 named,     runtime 0
    //   Uniflow(2)                      anonymous, runtime 2
    //   Uniflow("name", 2)              named,     runtime 2
    // A type may have at most one *anonymous* instance (auto-named after the
    // class); a second one throws — name them. Names are unique per type only.
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
                    + "' — give your instances explicit names");
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
        // the module is idle (no flow) until Start(), and the pump skips it.
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

    // ── IUniflowObject ──
    bool IsIdle() const override
    {
        std::lock_guard<std::mutex> lk(op_mu_);
        return !flow_running_;
    }
    bool ExecuteOnce(IUniflowObserver& obs) override;

    // ── Public control surface ──

    // Start a flow at entry step `fn`, forwarding `args` to it. The entry step
    // is the only step that may take parameters; UF_NEXT steps take none.
    // Returns false if a flow is already running. Thread-safe — serialised with
    // the pump by the per-module mutex; writes the caller makes before Start()
    // are visible to the flow.
    template <class... Params, class... Args>
    bool Start(StepResult (Derived::*fn)(Params...), Args&&... args)
    {
        std::lock_guard<std::mutex> lk(op_mu_);
        if (flow_running_)
            return false;

        Derived* self = static_cast<Derived*>(this);
        // Decay-copy the args into the thunk so the entry step can be re-run
        // (a Stay) and never reaches back into the caller's storage.
        entry_thunk_ =
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
        curr_fn_              = nullptr;
        curr_step_name_       = nullptr;
        curr_ordinal_         = 0;
        flow_started_at_      = Clock::now();
        total_cpu_            = {};
        total_async_          = {};
        async_pending_        = false;
        slow_warned_          = false;
        wake_pending_         = false;
        trace_.clear();
        flow_started_pending_ = true;
        return true;
    }

    // Block the calling thread until the running flow finishes (returns at once
    // if idle). Call from the owning thread — never from inside a step.
    void Wait()
    {
        std::unique_lock<std::mutex> lk(op_mu_);
        idle_cv_.wait(lk, [this] { return !flow_running_; });
    }

    const std::string& InstanceName() const { return instance_name_; }
    int                RuntimeIndex() const { return runtime_index_; }

    // ── Cross-module access ──
    // Fetch another module of this type by name (or the anonymous one). Safe
    // only between modules on the *same* runtime — a debug assert guards it.
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
               && "uniflow: cross-runtime GetInst — access is unsafe across "
                  "pump threads");
        return *p;
    }
    static Derived& GetInst() { return GetInst(Derived::uf_class_name_()); }

protected:
    // ── Helpers your step functions return ──
    static StepResult Stay() { return {StepAction::Stay, nullptr, nullptr}; }
    static StepResult Done() { return {StepAction::Done, nullptr, nullptr}; }
    static StepResult Fail() { return {StepAction::Fail, nullptr, nullptr}; }
    // Re-run this same step, but not before `d` has elapsed. The paced
    // counterpart of Stay() — for "poll again soon" or "let motion advance"
    // without the pump busy-spinning.
    static StepResult Sleep(Duration d)
    {
        return {StepAction::Wait, nullptr, nullptr, d};
    }
    // Advance to `fn`. Not called directly — UF_NEXT fills in pointer + name.
    static StepResult Next(StepFn fn, const char* name)
    {
        return {StepAction::Advance, fn, name};
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
    // Per-type instance registry — distinct for each Uniflow<Derived>, since
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
        curr_fn_        = nullptr;
        curr_step_name_ = nullptr;
        entry_thunk_    = nullptr;
        curr_ordinal_   = 0;
        async_pending_  = false;
        slow_warned_    = false;
        wake_pending_   = false;
    }

    std::string             instance_name_;
    int                     runtime_index_ = 0;
    bool                    is_anonymous_  = false;
    detail::UniflowRuntime* runtime_       = nullptr;

    // Serialises an external Start()/Wait() against the pump's ExecuteOnce().
    // Mutable so the const IsIdle() can lock it.
    mutable std::mutex      op_mu_;
    std::condition_variable idle_cv_; // notified when a flow ends (for Wait)

    // ── Current position within the running flow ──
    bool        flow_running_         = false;
    StepFn      curr_fn_              = nullptr; // steady step to run next tick
    const char* curr_step_name_       = nullptr; // its name (for logs)
    int         curr_ordinal_         = 0;       // steps run so far this flow
    TimePoint   flow_started_at_      = {};
    Duration    total_cpu_            = {};      // summed pump-thread step time
    Duration    total_async_          = {};      // summed pool wait time
    bool        flow_started_pending_ = false;   // defer OnFlowStarted one tick
    bool        wake_pending_         = false;   // a Sleep() delay is in effect
    TimePoint   wake_at_              = {};      // when the Sleep()'d step re-runs

    // The entry step, held as a thunk because it may carry bound arguments.
    // Non-empty only until the entry step advances; steady steps use curr_fn_.
    std::function<StepResult()> entry_thunk_;

    // ── Async slot: state of the one in-flight UF_ASYNC submission ──
    bool                  async_pending_ = false;
    bool                  slow_warned_   = false;
    std::future<std::any> pending_fut_;
    const char*           pending_job_label_ = nullptr;
    AsyncOpts             pending_opts_;
    TimePoint             pending_submitted_at_;

    // ── Last async result — written on completion, read via AsyncResult<T>() ──
    std::any           last_async_value_;
    std::exception_ptr last_async_exception_;
    bool               last_async_was_timeout_ = false;

    // ── Trace + cross-flow stats handed to OnFlowEnded ──
    std::vector<TraceEntry> trace_;
    FlowStats               stats_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  Uniflow<Derived> — out-of-line definitions (need detail::Hub complete)
// ─────────────────────────────────────────────────────────────────────────────

// ExecuteOnce advances this module by one tick. The pump calls it round-robin
// on every non-idle module. One tick does at most one of:
//   - nothing yet (an async job it waits on has not finished), or
//   - run exactly one step and act on what that step returned.
// It never blocks: a not-ready async job makes it return at once.
template <class Derived>
bool Uniflow<Derived>::ExecuteOnce(IUniflowObserver& obs)
{
    // One lock for the whole tick, pairing with Start()/Wait()'s lock.
    std::lock_guard<std::mutex> lk(op_mu_);

    // Start() only set a flag; emit OnFlowStarted here so it stays ordered
    // with the step events the pump thread produces.
    if (flow_started_pending_)
    {
        obs.OnFlowStarted(instance_name_);
        flow_started_pending_ = false;
    }
    if (!flow_running_)
        return false; // idle — nothing to do

    // ── 1. Gate on pending async ──
    // If a previous step submitted async work, do not run the continuation
    // step until that work finishes (or times out).
    if (async_pending_)
    {
        auto now       = Clock::now();
        auto elapsed   = now - pending_submitted_at_;
        // wait_for(0) just polls — it never blocks the pump thread.
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
            return false; // no step ran — the pump may sleep
        }

        // Ready, or blew its deadline → move the outcome into the result slot.
        if (timed_out && !ready)
        {
            last_async_value_.reset();
            last_async_exception_ = std::make_exception_ptr(
                AsyncTimeout{pending_job_label_, elapsed});
            last_async_was_timeout_ = true;
        }
        else
        {
            // future::get() rethrows whatever the worker threw — a throw
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

        // Record the wait as its own trace entry — kept separate from step
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

    // ── 1b. Honour a Sleep(): hold the step until its delay elapses ──
    if (wake_pending_)
    {
        if (Clock::now() < wake_at_)
            return false; // not due yet — no step ran, the pump may sleep
        wake_pending_ = false;
    }

    // ── 2. Run the step (timed) ──
    StepResult  r;
    const char* step_name = nullptr;
    auto        t0        = Clock::now();
    if (entry_thunk_)
    {
        step_name = "(entry)";
        r         = entry_thunk_();
    }
    else
    {
        step_name = curr_step_name_;
        r         = (static_cast<Derived*>(this)->*curr_fn_)();
    }
    auto cpu_dt = Clock::now() - t0;

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

    // A step shares the one pump thread with every other module — if it
    // overran the threshold, raise the core single-thread-protection alarm.
    if (cpu_dt > detail::Hub::get().config().slow_cpu_threshold)
        obs.OnSlowCpuStep(instance_name_, step_name ? step_name : "", cpu_dt);

    // If this very step called UF_ASYNC, async_pending_ is now set.
    if (async_pending_)
        obs.OnAsyncSubmitted(instance_name_, step_name ? step_name : "",
                             pending_job_label_ ? pending_job_label_ : "");

    // ── 3. Apply the StepResult ──
    switch (r.action)
    {
    case StepAction::Stay:
        // Cursor unchanged: the same step (entry thunk or curr_fn_) re-runs.
        break;
    case StepAction::Wait:
        // Cursor unchanged, but hold the re-run until the delay elapses.
        wake_pending_ = true;
        wake_at_      = Clock::now() + r.delay;
        break;
    case StepAction::Advance:
        entry_thunk_    = nullptr; // leave entry mode, if we were in it
        curr_fn_        = r.next_fn;
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
// non-type template parameter — the actual function baked in at compile time,
// so the worker lambda calls it with zero indirection. UF_ASYNC fills it in.
template <class Derived>
template <auto Fn, class... Args>
void Uniflow<Derived>::SubmitAsync(const char* job_label, AsyncOpts opts,
                                   Args&&... args)
{
    // Compile-time safety gate: the target must NOT be a non-static member
    // function. A worker on another thread must never touch `this` —
    // everything it needs is passed explicitly through args.
    static_assert(!std::is_member_function_pointer_v<decltype(Fn)>,
                  "UF_ASYNC target must be a `static` member or free function. "
                  "Instance state is unsafe across threads — pass needed data "
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

    // Decay-copy the args into a value tuple — the worker can never reach
    // back into `this` even if the caller passed a member reference.
    auto tup = std::tuple<std::decay_t<Args>...>(std::forward<Args>(args)...);

    detail::Hub::get().executor(pending_opts_.executor_name).Submit(
        [promise, tup = std::move(tup)]() mutable
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
        });
}

} // namespace uniflow

// ─────────────────────────────────────────────────────────────────────────────
//  Convenience macros
//
//  Place UF_USES_UNIFLOW(MyClass) — or UF_SINGLETON(MyClass) — once at the top
//  of the class body. Both leave the class in a `private:` section, so add
//  `public:` before your entry steps. The macros capture, for every step /
//  worker function, both `&S::fn` (the pointer) and `#fn` (its name string) —
//  which is why step names appear in logs with no manual logging in your code.
// ─────────────────────────────────────────────────────────────────────────────

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

// Advance to step `fn`: builds a StepResult carrying the pointer + name.
#define UF_NEXT(fn) this->Next(&S::fn, #fn)

// Submit static function `fn` to the "default" pool with default options.
// `, ##__VA_ARGS__` is the GNU comma-elision form: the leading comma is
// dropped when no extra args are passed. GCC, Clang, and the MSVC
// preprocessor all honour it — no special compiler flag needed.
#define UF_ASYNC(fn, ...)                                                     \
    this->template SubmitAsync<&S::fn>(#fn, ::uniflow::AsyncOpts{},           \
                                       ##__VA_ARGS__)

// Same as UF_ASYNC but with an explicit AsyncOpts (timeout / pool / warn).
#define UF_ASYNC_OPT(fn, opts, ...)                                           \
    this->template SubmitAsync<&S::fn>(#fn, (opts), ##__VA_ARGS__)
