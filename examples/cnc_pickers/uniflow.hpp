// uniflow.hpp - single-threaded, step-driven cooperative async framework.
// Single header, C++17.
//
// A module is a class deriving from Uniflow<Derived>; its logic is a chain
// of step functions returning StepResult. One pump thread per Runtime drives
// every attached module round-robin; blocking work goes to a thread pool
// via UF_ASYNC. No framework-side global state - the user creates a Runtime
// and constructs modules with 'Runtime&'.
//
//   class OrderRouter : public uniflow::Uniflow<OrderRouter> {
//       UF_UNIFLOW_IMPLEMENT(OrderRouter);
//   public:
//       explicit OrderRouter(uniflow::Runtime& rt)
//           : uniflow::Uniflow<OrderRouter>(rt) {}
//       StepResult OnRoute_Begin(Message m) { msg_ = std::move(m);
//                                             return UF_NEXT(OnRoute_Done); }
//   private:
//       StepResult OnRoute_Done() { return Done(); }
//       Message msg_;
//   };
//
//   uniflow::Runtime rt;
//   OrderRouter      router{rt};
//   UF_START_FLOW(router, OnRoute_Begin, Message{...});
//   router.WaitUntilIdle();
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
#include <utility>
#include <vector>


namespace uniflow
{

// ----- Time -----
using Clock     = std::chrono::steady_clock;
using TimePoint = Clock::time_point;
using Duration  = Clock::duration;

inline double to_ms(Duration d)
{
    return std::chrono::duration<double, std::milli>(d).count();
}

// ----- StepAction: a step returns an intent, not a state change -----
// Four intents only: Stay, Next, Done, Fail. Stay carries an optional
// per-module gate delay (default zero -> pump idle_sleep cadence).
enum class StepAction : uint8_t
{
    Stay, // re-run the same step (optional per-module gate via Stay(d))
    Next, // advance to result.next_fn on the next pump round
    Done, // flow completed normally -> idle
    Fail  // flow aborted -> idle
};

inline const char* to_string(StepAction a)
{
    switch (a)
    {
    case StepAction::Stay: return "Stay";
    case StepAction::Next: return "Next";
    case StepAction::Done: return "Done";
    case StepAction::Fail: return "Fail";
    }
    return "?";
}

// ----- Async support -----

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
// '_val' points into the owning module's slot - use it before the step
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

// ----- Executor abstraction -----
class IExecutor
{
public:
    virtual ~IExecutor()                              = default;
    virtual void   Submit(std::function<void()> task) = 0;
    virtual size_t QueueDepth() const                 = 0;
};

// Runs the task synchronously on the calling thread. Useful for tests - the
// whole flow becomes deterministic. NOT for production: it blocks the pump
// thread inside UF_ASYNC.
class InlineExecutor : public IExecutor
{
public:
    void   Submit(std::function<void()> task) override { task(); }
    size_t QueueDepth() const override { return 0; }
};

// ----- Trace + observer -----
// One TraceEntry per STEP (not per tick) - Stay re-entries collapse into a
// single entry recorded when the step finally Next/Done/Fail'd.
// 'ticks' = how many times the body ran during that step. Kept for
// inspection by custom observers; the bundled ConsoleObserver does not
// print it because tick counts rarely help with judgement at a glance.
struct TraceEntry
{
    enum class Kind { StepTransition, AsyncCompletion };
    Kind        kind;           // which of the two timing fields applies
    int         ordinal = 0;    // step number within the flow
    std::string name;           // step name or async job label
    int         ticks    = 0;   // StepTransition: body invocations in this step
    double      step_ms  = 0.0; // StepTransition: wall time spent in this step
    double      async_ms = 0.0; // AsyncCompletion: pool wait time in ms
    bool        had_error = false;
    bool        timed_out = false;
    StepAction  action    = StepAction::Stay; // StepTransition: terminal action
};

// Counters that persist across runs of a flow on the same module. max_seen_
// length drives the "reached #N/#M" log: N this run, M the longest ever.
struct FlowStats
{
    int         last_success_length = 0;
    int         max_seen_length     = 0;
    std::size_t success_count       = 0;
    std::size_t fail_count          = 0;
};

// Origin record: where in the source did StartFlow get called?
// Captured by the UF_START_FLOW macro (__FILE__ / __LINE__) and threaded
// through OnFlowStarted / OnFlowEnded so logs can answer 'who started this
// flow?'.
struct FlowOrigin
{
    const char* file = nullptr; // nullptr when StartFlow was called raw
    int         line = 0;
};

// Every log line / metric the framework produces is funnelled through one of
// these hooks - the framework itself never touches std::cout. Subclass and
// pass via Runtime::Opts::observer; override only the events you care about.
//
// All time arguments are double milliseconds, rounded at the framework
// boundary. Internal accounting still uses chrono::duration; the conversion
// happens once here so user observers do not need to know about Duration.
class IUniflowObserver
{
public:
    virtual ~IUniflowObserver() = default;

    // Fired once when StartFlow() arms a fresh flow on 'obj'. Called on the
    // pump thread driving that module, before any step runs. 'origin' is the
    // source location of the UF_START_FLOW caller (file/line = {nullptr,0}
    // if StartFlow was called without the macro).
    virtual void OnFlowStarted(std::string_view /*obj*/,
                               FlowOrigin       /*origin*/) {}

    // Fired once per STEP (not per tick), at the moment the step transitions
    // away - either Next to a different step, or Done / Fail terminating
    // the flow. Reports the step that JUST FINISHED, with totals accumulated
    // over its entire lifetime (sum of every Stay re-entry plus the final
    // return):
    //
    //   'step_ordinal'   = 0-based index of this step within the flow.
    //   'ticks_in_step'  = how many times the body was invoked during this
    //                      step (Stay re-entries plus the final call).
    //   'elapsed_ms'     = wall time from step entry until this transition
    //                      (the 'how long did we sit in this step?' answer).
    //   'description'    = the last value the step set via Describe(...).
    //
    // Use this for high-signal flow logs. There is no per-tick callback by
    // design - a Stay loop iterating thousands of times no longer floods
    // the observer.
    virtual void OnStepChanged(std::string_view /*obj*/, std::string_view /*step*/,
                               std::string_view /*description*/,
                               int /*step_ordinal*/, int /*ticks_in_step*/,
                               double /*elapsed_ms*/) {}

    // Fired when a step body throws. 'what' is the exception's what() (or
    // 'unknown' for non-std exceptions). The flow is forcibly Failed right
    // after this hook returns; the exception does NOT propagate out of the
    // pump thread.
    virtual void OnStepThrew(std::string_view /*obj*/, std::string_view /*step*/,
                             std::string_view /*what*/,
                             int /*step_ordinal*/, int /*tick*/) {}

    // Fired right after UF_ASYNC successfully enqueues 'job' to the pool.
    virtual void OnAsyncSubmitted(std::string_view /*obj*/, std::string_view /*step*/,
                                  std::string_view /*job*/) {}

    // Fired once per async job, after the worker either returned a value,
    // threw, or missed its deadline.
    virtual void OnAsyncCompleted(std::string_view /*obj*/, std::string_view /*job*/,
                                  double /*wait_ms*/, bool /*had_error*/,
                                  bool /*timed_out*/) {}

    // Single-thread-protection alarm: a step held the pump thread longer
    // than Config::slow_cpu_threshold this single body invocation.
    // Set the threshold to Duration::max() to disable.
    virtual void OnSlowCpuStep(std::string_view /*obj*/, std::string_view /*step*/,
                               double /*cpu_ms*/) {}

    // Fired at most once per async job, when its in-flight time crosses
    // Config::slow_async_threshold. Set the threshold to Duration::max()
    // (the default) to disable.
    virtual void OnSlowAsync(std::string_view /*obj*/, std::string_view /*job*/,
                             double /*wait_so_far_ms*/) {}

    // Fired once when a flow leaves the running state via Done or Fail.
    //   'terminal_action'    - Done or Fail
    //   'final_step_ordinal' - logical step count reached (Next-only)
    //   'total_ticks'        - total step body invocations (incl. re-entries)
    //   'trace'              - ordered StepTransition + AsyncCompletion log
    //   'wall_ms'            - flow start -> end wall time, ms
    //   'total_step_ms'      - summed pump-thread time across every step
    //                          body invocation, ms
    //   'total_async_ms'     - summed time the flow spent gated on async, ms
    //   'origin'             - UF_START_FLOW source location
    virtual void OnFlowEnded(std::string_view /*obj*/, StepAction /*terminal_action*/,
                             int /*final_step_ordinal*/, int /*total_ticks*/,
                             const std::vector<TraceEntry>& /*trace*/,
                             double /*wall_ms*/, double /*total_step_ms*/,
                             double /*total_async_ms*/,
                             const FlowStats& /*stats*/,
                             FlowOrigin       /*origin*/) {}
};

// Default observer - pretty-prints to stdout in fixed-width columns. Thread-
// safe: every Runtime's pump thread may call into it.
//
// Column layout (STEP rows):
//   [ obj          ] step                       description                     #s elapsed
//   [ Stage        ] OnProcess_WaitHwReady      hw ready handshake settling     #04 elapsed=0.02ms
class ConsoleObserver : public IUniflowObserver
{
public:
    static constexpr int kColObj  = 14;
    static constexpr int kColStep = 28;
    static constexpr int kColDesc = 36;

    void OnFlowStarted(std::string_view obj, FlowOrigin origin) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << pad(obj, kColObj) << "] FLOW START";
        if (origin.file)
            std::cout << "  caller=" << origin.file << ":" << origin.line;
        std::cout << "\n";
    }
    void OnStepChanged(std::string_view obj, std::string_view step,
                       std::string_view description,
                       int step_ordinal, int /*ticks_in_step*/,
                       double elapsed_ms) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << pad(obj, kColObj) << "] "
                  << pad(step, kColStep) << " "
                  << pad(description, kColDesc) << " "
                  << "#" << pad2(step_ordinal)
                  << " elapsed=" << fmt_ms(elapsed_ms) << "\n";
    }
    void OnStepThrew(std::string_view obj, std::string_view step,
                     std::string_view what,
                     int step_ordinal, int /*tick*/) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << pad(obj, kColObj) << "] "
                  << pad(step, kColStep) << " "
                  << "[THREW] " << what
                  << " #" << pad2(step_ordinal) << "\n";
    }
    void OnAsyncSubmitted(std::string_view obj, std::string_view step,
                          std::string_view job) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << pad(obj, kColObj) << "] "
                  << pad(step, kColStep) << " "
                  << "ASYNC SUBMIT  " << job << "\n";
    }
    void OnAsyncCompleted(std::string_view obj, std::string_view job,
                          double wait_ms, bool had_error, bool timed_out) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << pad(obj, kColObj) << "] "
                  << pad("", kColStep) << " "
                  << "ASYNC DONE    " << job
                  << "  wait=" << fmt_ms(wait_ms);
        if (timed_out)      std::cout << "  [TIMEOUT]";
        else if (had_error) std::cout << "  [ERROR]";
        std::cout << "\n";
    }
    void OnSlowCpuStep(std::string_view obj, std::string_view step,
                       double cpu_ms) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << pad(obj, kColObj) << "] "
                  << pad(step, kColStep) << " "
                  << "[SLOW CPU]  step held pump for " << fmt_ms(cpu_ms) << "\n";
    }
    void OnSlowAsync(std::string_view obj, std::string_view job,
                     double wait_so_far_ms) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << pad(obj, kColObj) << "] "
                  << pad("", kColStep) << " "
                  << "[SLOW ASYNC]  " << job
                  << "  pending=" << fmt_ms(wait_so_far_ms) << "\n";
    }
    void OnFlowEnded(std::string_view obj, StepAction terminal_action,
                     int final_step_ordinal, int total_ticks,
                     const std::vector<TraceEntry>& /*trace*/,
                     double wall_ms, double total_step_ms,
                     double total_async_ms, const FlowStats& /*stats*/,
                     FlowOrigin origin) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        (void)total_ticks;
        std::cout << "[" << pad(obj, kColObj) << "] FLOW "
                  << (terminal_action == StepAction::Done ? "END  DONE  " : "END  FAIL  ")
                  << "steps=#" << pad2(final_step_ordinal)
                  << "  wall=" << fmt_ms(wall_ms)
                  << "  step=" << fmt_ms(total_step_ms)
                  << "  async=" << fmt_ms(total_async_ms);
        if (origin.file)
            std::cout << "  caller=" << origin.file << ":" << origin.line;
        std::cout << "\n";
    }

private:
    static std::string pad(std::string_view sv, int width)
    {
        std::string s(sv);
        if (static_cast<int>(s.size()) < width)
            s.append(width - s.size(), ' ');
        return s;
    }
    static std::string pad2(int v)
    {
        std::ostringstream os;
        os << std::setw(2) << std::setfill('0') << v;
        return os.str();
    }
    static std::string fmt_ms(double ms)
    {
        std::ostringstream os;
        os << std::fixed << std::setprecision(2) << ms << "ms";
        return os.str();
    }
    std::mutex mu_;
};

// ----- Config: per-Runtime tuning (pass via Runtime::Opts) -----
struct Config
{
    // Pump-thread rest period when NO flow is running anywhere on this
    // Runtime (every module is idle - Done / Fail terminated or never
    // started). Short by default so a fresh StartFlow() is picked up
    // quickly without burning CPU.
    Duration idle_sleep = std::chrono::milliseconds(1);

    // Pump-thread rest period when flows ARE running but every active
    // module returned Stay or was gated this round (no Next / Done /
    // Fail). Longer than idle_sleep because steady-state polling rarely
    // needs sub-ms reaction - 20ms keeps CPU near zero while still
    // feeling responsive.
    Duration stay_sleep = std::chrono::milliseconds(20);

    // Pump-thread rest period between rounds when at least one module
    // advanced this round (Next / Done / Fail). Default 0 = burst mode:
    // chains of Next steps run as fast as the pump can dispatch them.
    // Set to e.g. 1ms to trade a little latency for less CPU during
    // heavy bursts.
    Duration step_interval_sleep = Duration::zero();

    // OnSlowCpuStep alarm threshold. If a single step body invocation
    // holds the pump thread for longer than this, OnSlowCpuStep fires
    // once for that invocation. Set to Duration::max() to disable.
    Duration slow_cpu_threshold   = std::chrono::milliseconds(5);

    // OnSlowAsync alarm threshold. Fires once per async job, the first
    // time the job's in-flight time crosses this. Default disabled - opt
    // in by setting a finite value when you actually care.
    Duration slow_async_threshold = Duration::max();
};

// ----- detail: hidden machinery -----
namespace detail
{

// Set once per pump thread to the Runtime's process-wide index; -1 on any
// other thread. Useful for user-side asserts that catch cross-runtime
// access (calling foo.something() from a step on a different Runtime is
// unsafe because foo is being driven by a different pump thread).
inline thread_local int t_runtime_idx = -1;

// Monotonic counter so each Runtime gets a stable distinct index.
inline std::atomic<int>& next_runtime_index()
{
    static std::atomic<int> n{0};
    return n;
}

// Uniflow<Derived> is a template, so a Runtime cannot hold "all modules"
// directly - each module is a different type. IUniflowObject is the
// non-template base they share, so the pump can drive them uniformly.
class IUniflowObject
{
public:
    virtual ~IUniflowObject()                   = default;
    virtual bool IsIdle() const                 = 0;
    // Run one pump tick on this module. Returns true iff the step body
    // ran AND its result was a transition (Next / Done / Fail). Stay
    // results, gated-on-async, and gated-on-Stay(d) all return false so
    // the pump can decide to sleep idle_sleep when nothing advanced.
    // 'force_wake' makes the module ignore its in-flight Stay(d) gate
    // this round - the pump sets it when an external Notify() told us
    // the world changed, so the step re-evaluates its condition right
    // away rather than wait the gate interval out.
    virtual bool ExecuteOnce(IUniflowObserver&, bool force_wake) = 0;
};

// Forward-declared factory; defined at the bottom of this header, after the
// BS::thread_pool body, because the default executor wraps that pool.
std::unique_ptr<IExecutor> make_default_executor(std::size_t threads);

} // namespace detail

// ======================================================================
//  Runtime - user-owned, exactly one per pump thread.
//
//  Construction spins up the pump thread, wires up the executor (default:
//  BS::thread_pool with 'Opts::threads' workers), the observer (default:
//  ConsoleObserver), and the config. Destruction stops the pump and joins.
//
//  Modules attach to a Runtime via their 'Uniflow(Runtime&)' ctor; the same
//  Runtime drives every module attached to it.
//
//  Multi-runtime use:
//    - Create more than one 'Runtime' to get more than one pump thread.
//    - A module belongs to exactly one Runtime; cross-runtime access from a
//      step body is unsafe (no locks between modules on different pumps).
//      Use 'RuntimeIndex()' and 'detail::t_runtime_idx' for asserts.
// ======================================================================
class Runtime
{
public:
    struct Opts
    {
        // Worker thread count for the default executor. 0 picks
        // hardware_concurrency. Ignored if 'executor' is set.
        std::size_t                       threads = 0;

        // Override the executor. If null, a BS::thread_pool with 'threads'
        // workers is constructed internally.
        std::unique_ptr<IExecutor>        executor;

        // Override the observer. If null, a ConsoleObserver is used.
        std::unique_ptr<IUniflowObserver> observer;

        // Per-Runtime config.
        Config                            config{};
    };

    Runtime() : Runtime(Opts{}) {}
    explicit Runtime(Opts opts)
        : index_(detail::next_runtime_index().fetch_add(1)),
          config_(opts.config),
          executor_(opts.executor
                    ? std::move(opts.executor)
                    : detail::make_default_executor(opts.threads)),
          observer_(opts.observer
                    ? std::move(opts.observer)
                    : std::make_unique<ConsoleObserver>())
    {
        pump_ = std::thread([this] { pump_loop(); });
    }

    ~Runtime()
    {
        stop_.store(true, std::memory_order_relaxed);
        if (pump_.joinable())
            pump_.join();
    }

    Runtime(const Runtime&)            = delete;
    Runtime& operator=(const Runtime&) = delete;

    int               index()    const { return index_; }
    IExecutor&        executor()       { return *executor_; }
    IUniflowObserver& observer()       { return *observer_; }
    const Config&     config()   const { return config_; }

    // -- Called by Uniflow base; could be friended, but the surface is
    //    small enough to keep public without leaking machinery to users --
    void attach(detail::IUniflowObject* m)
    {
        std::lock_guard<std::recursive_mutex> lk(objects_mu_);
        objects_.push_back(m);
    }
    void detach(detail::IUniflowObject* m)
    {
        std::lock_guard<std::recursive_mutex> lk(objects_mu_);
        objects_.erase(std::remove(objects_.begin(), objects_.end(), m),
                       objects_.end());
    }

    // Mark that an external event happened. The next pump round bypasses
    // every module's Stay(d) gate once so polling steps re-evaluate their
    // condition immediately instead of sleeping the gate interval out.
    // Called by:
    //   - any async worker right after it has set the result on its promise,
    //   - StartFlow() when a module transitions from idle to active,
    //   - external signallers that just flipped a condition some step
    //     is waiting on (e.g. an IRQ-style HW-ready callback).
    //
    // The pump itself is not woken from sleep; it sleeps at most
    // Config::idle_sleep between rounds, so external-wake latency is
    // bounded by that value.
    void Notify()
    {
        ext_wake_observed_.store(true, std::memory_order_release);
    }

    bool consume_external_wake()
    {
        return ext_wake_observed_.exchange(false, std::memory_order_acquire);
    }

private:
    // Pump policy: each round, run every non-idle module once. Sleep
    // between rounds is picked from one of THREE knobs based on what
    // happened this round:
    //   1. any module advanced (Next / Done / Fail)
    //        -> step_interval_sleep (default 0 = burst; chains of Next
    //           run as fast as the pump can dispatch)
    //   2. flows ARE running but every active module Stayed / was gated
    //        -> stay_sleep (default 20ms; steady-state polling cadence)
    //   3. no flows running at all (every module idle)
    //        -> idle_sleep (default 1ms; quick pickup of fresh StartFlow)
    // Any knob set to Duration::zero() skips sleep_for entirely.
    void pump_loop()
    {
        detail::t_runtime_idx = index_;
        for (;;)
        {
            if (stop_.load(std::memory_order_relaxed))
                return;
            bool force_wake   = consume_external_wake();
            bool any_active   = false;
            bool any_advanced = false;
            {
                std::lock_guard<std::recursive_mutex> lk(objects_mu_);
                for (std::size_t i = 0; i < objects_.size(); ++i)
                {
                    detail::IUniflowObject* o = objects_[i];
                    if (o->IsIdle())
                        continue;
                    any_active = true;
                    if (o->ExecuteOnce(*observer_, force_wake))
                        any_advanced = true;
                }
            }
            Duration nap;
            if (any_advanced)    nap = config_.step_interval_sleep;
            else if (any_active) nap = config_.stay_sleep;
            else                 nap = config_.idle_sleep;
            if (nap > Duration::zero())
                std::this_thread::sleep_for(nap);
        }
    }

    int                               index_;
    Config                            config_;
    std::unique_ptr<IExecutor>        executor_;
    std::unique_ptr<IUniflowObserver> observer_;

    std::recursive_mutex                 objects_mu_;
    std::vector<detail::IUniflowObject*> objects_;
    std::atomic<bool>                    stop_{false};
    std::atomic<bool>                    ext_wake_observed_{false};

    std::thread                          pump_;
};

// Two ways to wait in a step:
//   (1) UF_ASYNC: offload blocking work to the pool; pump is woken on
//       completion. Best for I/O, CPU work.
//   (2) UFTimer: poll a condition with a deadline. Best for HW flags or
//       peer state where there is no completion callback.
class UFTimer
{
public:
    enum Result : uint8_t { Waiting, Done, Timeout };

    UFTimer() : armed_at_(Clock::now()) {}

    void Restart() { armed_at_ = Clock::now(); }
    Duration Elapsed() const { return Clock::now() - armed_at_; }
    bool TimedOut(Duration deadline) const { return Elapsed() >= deadline; }

    template <class Predicate,
              std::enable_if_t<std::is_invocable_r_v<bool, Predicate>, int> = 0>
    Result OnWait(Predicate condition, Duration timeout) const
    {
        if (condition()) return Done;
        if (TimedOut(timeout)) return Timeout;
        return Waiting;
    }
    Result OnWait(bool condition, Duration timeout) const
    {
        if (condition) return Done;
        if (TimedOut(timeout)) return Timeout;
        return Waiting;
    }

private:
    TimePoint armed_at_;
};

// Uniflow<Derived> - CRTP base. Module: 'class Foo : public Uniflow<Foo>'
// + 'UF_UNIFLOW_IMPLEMENT(Foo)' + a ctor taking 'Runtime&'. No UF_SINGLETON
// - hold modules as members of your own App and expose 'App::inst()'.
template <class Derived>
class Uniflow : public detail::IUniflowObject
{
public:
    // What a step returns: an intent, not a state change.
    struct StepResult
    {
        StepAction                  action    = StepAction::Stay;
        std::function<StepResult()> next_fn;             // set only when Next
        const char*                 next_name = nullptr; // step name for the log
        Duration                    delay     = {};      // optional Stay gate
    };

    // -- Constructors --
    //   Uniflow(rt)         anonymous (auto-named after the class)
    //   Uniflow(rt, "name") named
    // The base attaches the module to 'rt' and starts driving it on the
    // next pump round (initially idle - no flow until StartFlow()).
    explicit Uniflow(Runtime& rt) : Uniflow(rt, nullptr) {}
    Uniflow(Runtime& rt, const char* name)
        : runtime_(&rt)
    {
        instance_name_ = name ? std::string(name) : Derived::uf_class_name_();
        runtime_->attach(this);
    }

    ~Uniflow() override
    {
        // Destroying a module mid-flow is a use-after-free hazard (the pump
        // may be in its step). Contract: destroy modules only when idle.
        assert(!flow_running_ && "uniflow: module destroyed while a flow runs");
        if (runtime_)
            runtime_->detach(this);
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

    // Start a flow at entry step 'fn', forwarding 'args' to it.
    template <class... Params, class... Args>
    bool StartFlow(StepResult (Derived::*fn)(Params...), Args&&... args)
    {
        return StartFlowAt(nullptr, 0, fn, std::forward<Args>(args)...);
    }
    template <class... Params, class... Args>
    bool StartFlowAt(const char* origin_file, int origin_line,
                     StepResult (Derived::*fn)(Params...), Args&&... args)
    {
        std::lock_guard<std::mutex> lk(op_mu_);
        if (flow_running_)
            return false;

        Derived* self = static_cast<Derived*>(this);
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

        auto now              = Clock::now();
        flow_running_         = true;
        curr_step_name_       = "(entry)";
        curr_step_description_.clear();
        step_ordinal_         = 0;
        tick_count_           = 0;
        ticks_in_step_        = 0;
        flow_started_at_      = now;
        step_started_at_      = now;
        total_cpu_            = {};
        total_async_          = {};
        async_pending_        = false;
        slow_warned_          = false;
        wake_pending_         = false;
        trace_.clear();
        flow_started_pending_ = true;
        flow_origin_          = FlowOrigin{origin_file, origin_line};

        if (runtime_)
            runtime_->Notify();
        return true;
    }

    // Block the calling thread until the running flow finishes (returns at
    // once if already idle). Call from the OWNING thread - never from inside
    // a step.
    void WaitUntilIdle()
    {
        std::unique_lock<std::mutex> lk(op_mu_);
        idle_cv_.wait(lk, [this] { return !flow_running_; });
    }

    const std::string& InstanceName() const { return instance_name_; }
    int                RuntimeIndex() const { return runtime_->index(); }
    const std::string& CurrentStepDescription() const { return curr_step_description_; }

protected:
    // -- The four step intents. Return one from every step body. --
    //
    // Stay()   - re-run this step on the next pump round. No per-module
    //            gate; the pump's idle_sleep cadence applies between
    //            rounds when every module is staying.
    // Stay(d)  - re-run this step after at least 'd' has elapsed. Per-
    //            module gate; Runtime::Notify() bypasses it once.
    // Next(fn) - advance to step 'fn' on the next pump round, with no
    //            sleep when other modules are also advancing. Called
    //            through the UF_NEXT(fn, args...) macro so the step name
    //            shows up in logs.
    // Done()   - flow finished normally; module goes idle.
    // Fail()   - flow aborted; module goes idle.
    StepResult Stay()           { return {StepAction::Stay, {}, nullptr, {}}; }
    StepResult Stay(Duration d) { return {StepAction::Stay, {}, nullptr, d}; }
    StepResult Done()           { return {StepAction::Done, {}, nullptr, {}}; }
    StepResult Fail()           { return {StepAction::Fail, {}, nullptr, {}}; }

    template <auto Fn, class... Args>
    StepResult Next(const char* name, Args&&... args)
    {
        Derived* self = static_cast<Derived*>(this);
        if constexpr (sizeof...(Args) == 0)
        {
            return {StepAction::Next,
                    [self]() -> StepResult { return (self->*Fn)(); },
                    name, {}};
        }
        else
        {
            auto tup = std::tuple<std::decay_t<Args>...>(
                std::forward<Args>(args)...);
            return {StepAction::Next,
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

    // Per-module exception policy. Override in Derived to control what
    // happens when a step body throws:
    //   return false (default) - rethrow (std::terminate); fail-fast crash.
    //   return true             - log via OnStepThrew, end the flow as Fail,
    //                             pump survives.
    bool CatchStepExceptions() const { return false; }

    // Bypass every module's Stay(d) gate once on the next pump round.
    // Use from a non-step method that just flipped a condition some peer
    // module's step is polling - e.g. a state mutator called by another
    // module's step.
    void NotifyRuntime() { if (runtime_) runtime_->Notify(); }

    // Attach a human-readable description to the current step.
    template <class... Args>
    void Describe(Args&&... args)
    {
        std::ostringstream os;
        (os << ... << std::forward<Args>(args));
        curr_step_description_ = os.str();
    }

    // Submit a static function to the Runtime's executor. Use UF_ASYNC /
    // UF_ASYNC_TIMEOUT. 'timeout' of Duration::max() means no deadline.
    template <auto Fn, class... Args>
    void SubmitAsync(const char* job_label, Duration timeout, Args&&... args);

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
    // Reset to idle once a flow reaches Done / Fail. Also wakes the runtime
    // so any module waiting on this one's idleness (e.g. the orchestrator)
    // observes the transition immediately, not on the next idle_sleep tick.
    void ClearFlow()
    {
        flow_running_   = false;
        curr_fn_        = nullptr;
        curr_step_name_ = nullptr;
        curr_step_description_.clear();
        step_ordinal_   = 0;
        tick_count_     = 0;
        ticks_in_step_  = 0;
        async_pending_  = false;
        slow_warned_    = false;
        wake_pending_   = false;
        flow_origin_    = FlowOrigin{};
        if (runtime_)
            runtime_->Notify();
    }

    std::string instance_name_;
    Runtime*    runtime_ = nullptr;

    // Serialises external StartFlow()/WaitUntilIdle() against pump ExecuteOnce().
    mutable std::mutex      op_mu_;
    std::condition_variable idle_cv_;

    // -- Current position within the running flow --
    bool        flow_running_         = false;
    const char* curr_step_name_       = nullptr;
    std::string curr_step_description_;
    int         step_ordinal_         = 0;
    int         tick_count_           = 0;       // total body invocations in flow
    int         ticks_in_step_        = 0;       // body invocations in current step
    TimePoint   flow_started_at_      = {};
    TimePoint   step_started_at_      = {};      // wall-clock start of current step
    Duration    total_cpu_            = {};
    Duration    total_async_          = {};
    bool        flow_started_pending_ = false;
    bool        wake_pending_         = false;
    TimePoint   wake_at_              = {};
    FlowOrigin  flow_origin_          = {};

    std::function<StepResult()> curr_fn_;

    // -- Async slot: state of the one in-flight UF_ASYNC submission --
    bool                  async_pending_ = false;
    bool                  slow_warned_   = false;  // OnSlowAsync fired once
    std::future<std::any> pending_fut_;
    const char*           pending_job_label_ = nullptr;
    Duration              pending_timeout_   = Duration::max();
    TimePoint             pending_submitted_at_;

    // -- Last async result - written on completion, read via AsyncResult<T>() --
    std::any           last_async_value_;
    std::exception_ptr last_async_exception_;
    bool               last_async_was_timeout_ = false;

    // -- Trace + cross-flow stats handed to OnFlowEnded --
    std::vector<TraceEntry> trace_;
    FlowStats               stats_;
};

// ----- Uniflow<Derived>: out-of-line definitions -----

template <class Derived>
bool Uniflow<Derived>::ExecuteOnce(IUniflowObserver& obs, bool force_wake)
{
    std::lock_guard<std::mutex> lk(op_mu_);

    if (flow_started_pending_)
    {
        obs.OnFlowStarted(instance_name_, flow_origin_);
        flow_started_pending_ = false;
    }
    if (!flow_running_)
        return false;

    const Config& cfg = runtime_->config();

    // -- 1. Gate on pending async --
    if (async_pending_)
    {
        auto now       = Clock::now();
        auto elapsed   = now - pending_submitted_at_;
        bool ready     = pending_fut_.wait_for(std::chrono::seconds(0))
                             == std::future_status::ready;
        bool timed_out = (elapsed > pending_timeout_);

        if (!ready && !timed_out)
        {
            // Slow-async alarm: fire once per job when the in-flight time
            // first crosses the configured threshold.
            if (!slow_warned_
                && cfg.slow_async_threshold != Duration::max()
                && elapsed > cfg.slow_async_threshold)
            {
                obs.OnSlowAsync(instance_name_,
                                pending_job_label_ ? pending_job_label_ : "",
                                to_ms(elapsed));
                slow_warned_ = true;
            }
            return false; // still gated; the pump may sleep
        }

        if (timed_out && !ready)
        {
            last_async_value_.reset();
            last_async_exception_ = std::make_exception_ptr(
                AsyncTimeout{pending_job_label_, elapsed});
            last_async_was_timeout_ = true;
        }
        else
        {
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
        obs.OnAsyncCompleted(instance_name_, pending_job_label_,
                             to_ms(elapsed),
                             last_async_exception_ != nullptr,
                             last_async_was_timeout_);

        TraceEntry te;
        te.kind      = TraceEntry::Kind::AsyncCompletion;
        te.ordinal   = step_ordinal_;
        te.name      = pending_job_label_ ? pending_job_label_ : "";
        te.async_ms  = to_ms(elapsed);
        te.had_error = last_async_exception_ != nullptr;
        te.timed_out = last_async_was_timeout_;
        trace_.push_back(std::move(te));
        total_async_ += elapsed;

        async_pending_ = false;
        slow_warned_   = false;
    }

    // -- 1b. Honour a Stay(d) gate: hold the step until the delay elapses --
    if (wake_pending_)
    {
        if (!force_wake && Clock::now() < wake_at_)
            return false;
        wake_pending_ = false;
    }

    // -- 2. Run the step (timed) --
    const char* step_name = curr_step_name_;
    tick_count_++;
    ticks_in_step_++;
    auto        t0    = Clock::now();
    StepResult  r;
    bool        step_had_error = false;
    try
    {
        r = curr_fn_();
    }
    catch (...)
    {
        std::exception_ptr ep = std::current_exception();
        std::string throw_what;
        try { std::rethrow_exception(ep); }
        catch (const std::exception& e) { throw_what = e.what(); }
        catch (...)                     { throw_what = "(non-std exception)"; }

        obs.OnStepThrew(instance_name_, step_name ? step_name : "",
                        throw_what, step_ordinal_, tick_count_);

        if (!static_cast<Derived*>(this)->CatchStepExceptions())
            std::rethrow_exception(ep);

        step_had_error = true;
        r = StepResult{StepAction::Fail, {}, nullptr, {}};
    }
    auto cpu_dt = Clock::now() - t0;
    total_cpu_ += cpu_dt;

    // Slow-cpu alarm: fire when this single body invocation held the
    // pump thread for longer than the configured threshold.
    if (!step_had_error
        && cfg.slow_cpu_threshold != Duration::max()
        && cpu_dt > cfg.slow_cpu_threshold)
    {
        obs.OnSlowCpuStep(instance_name_, step_name ? step_name : "",
                          to_ms(cpu_dt));
    }

    if (!step_had_error && async_pending_)
        obs.OnAsyncSubmitted(instance_name_, step_name ? step_name : "",
                             pending_job_label_ ? pending_job_label_ : "");

    // -- 3. Apply the StepResult.
    //    On step transitions (Next / Done / Fail) we emit one trace entry
    //    and OnStepChanged for the step that JUST finished, with cumulative
    //    ticks_in_step_ and the wall time spent in this step. Stay keeps
    //    accumulating into the same step. The return value of ExecuteOnce
    //    is true iff a transition happened this round; the pump uses that
    //    to decide whether to sleep idle_sleep before the next round. --
    bool advanced = false;
    switch (r.action)
    {
    case StepAction::Stay:
        if (r.delay > Duration::zero())
        {
            wake_pending_ = true;
            wake_at_      = Clock::now() + r.delay;
        }
        break;
    case StepAction::Next:
    case StepAction::Done:
    case StepAction::Fail:
    {
        advanced = true;
        auto   transition_at = Clock::now();
        double step_wall_ms  = to_ms(transition_at - step_started_at_);

        TraceEntry te;
        te.kind      = TraceEntry::Kind::StepTransition;
        te.ordinal   = step_ordinal_;
        te.name      = step_name ? step_name : "";
        te.ticks     = ticks_in_step_;
        te.step_ms   = step_wall_ms;
        te.action    = r.action;
        te.had_error = step_had_error;
        trace_.push_back(std::move(te));

        obs.OnStepChanged(instance_name_, step_name ? step_name : "",
                          curr_step_description_,
                          step_ordinal_, ticks_in_step_, step_wall_ms);

        if (r.action == StepAction::Next)
        {
            curr_fn_        = std::move(r.next_fn);
            curr_step_name_ = r.next_name;
            curr_step_description_.clear();
            step_ordinal_++;
            ticks_in_step_  = 0;
            step_started_at_ = transition_at;
        }
        else
        {
            // Done / Fail - flow ends.
            if (r.action == StepAction::Done)
            {
                stats_.success_count++;
                stats_.last_success_length = step_ordinal_;
                if (step_ordinal_ > stats_.max_seen_length)
                    stats_.max_seen_length = step_ordinal_;
            }
            else
            {
                stats_.fail_count++;
            }
            obs.OnFlowEnded(instance_name_, r.action,
                            step_ordinal_, tick_count_, trace_,
                            to_ms(transition_at - flow_started_at_),
                            to_ms(total_cpu_),
                            to_ms(total_async_),
                            stats_, flow_origin_);
            ClearFlow();
            idle_cv_.notify_all();
        }
        break;
    }
    }
    return advanced;
}

// SubmitAsync hands a job to the Runtime's executor and arms the async slot.
template <class Derived>
template <auto Fn, class... Args>
void Uniflow<Derived>::SubmitAsync(const char* job_label, Duration timeout,
                                   Args&&... args)
{
    static_assert(!std::is_member_function_pointer_v<decltype(Fn)>,
                  "UF_ASYNC target must be a 'static' member or free function. "
                  "Instance state is unsafe across threads - pass needed data "
                  "through args.");

    using R = std::invoke_result_t<decltype(Fn), Args...>;

    auto promise          = std::make_shared<std::promise<std::any>>();
    pending_fut_          = promise->get_future();
    pending_job_label_    = job_label;
    pending_timeout_      = timeout;
    pending_submitted_at_ = Clock::now();
    async_pending_        = true;

    auto tup = std::tuple<std::decay_t<Args>...>(std::forward<Args>(args)...);
    Runtime* rt = runtime_;

    runtime_->executor().Submit(
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
                promise->set_exception(std::current_exception());
            }
            if (rt) rt->Notify();
        });
}

} // namespace uniflow

// Convenience macros. UF_UNIFLOW_IMPLEMENT injects the boilerplate every
// Uniflow subclass needs (inherit ctor, name typedef for UF_NEXT, friend
// the base, expose the class name for default-named instances).

#define UF_UNIFLOW_IMPLEMENT(Cls)                                             \
  public:                                                                     \
    using uniflow::Uniflow<Cls>::Uniflow;                                     \
                                                                              \
  private:                                                                    \
    using S = Cls;                                                            \
    friend class ::uniflow::Uniflow<Cls>;                                     \
    static const char* uf_class_name_() { return #Cls; }

// Advance to step 'fn', optionally passing arguments through.
#define UF_NEXT(fn, ...) \
    this->template Next<&S::fn>(#fn, ##__VA_ARGS__)

// Submit static function 'fn' to the Runtime's executor with no deadline.
#define UF_ASYNC(fn, ...)                                                     \
    this->template SubmitAsync<&S::fn>(#fn, ::uniflow::Duration::max(),       \
                                       ##__VA_ARGS__)

// Same as UF_ASYNC but with a timeout. 'dur' is a uniflow::Duration
// (chrono::duration of any unit; the chrono literals 1ms / 1s also work).
#define UF_ASYNC_TIMEOUT(fn, dur, ...)                                        \
    this->template SubmitAsync<&S::fn>(#fn, (dur), ##__VA_ARGS__)

// Launch a flow on 'mod' at entry step 'fn', recording the caller's source
// location so log lines and 'who started this flow?' debugging is trivial.
//
//   UF_START_FLOW(app.stage, OnProcess_Begin);
//   UF_START_FLOW(my_router, OnRoute_Begin, message, 42);
#define UF_START_FLOW(mod, fn, ...)                                           \
    ((mod).StartFlowAt(                                                       \
        __FILE__, __LINE__,                                                   \
        &std::remove_reference_t<decltype(mod)>::fn,                          \
        ##__VA_ARGS__))

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

// Default executor: BSThreadPoolExecutor wrapping BS::thread_pool, in
// uniflow::detail so user code never names it.
namespace uniflow
{
namespace detail
{

class BSThreadPoolExecutor : public IExecutor
{
public:
    explicit BSThreadPoolExecutor(std::size_t threads)
        : pool_(static_cast<BS::concurrency_t>(
              threads ? threads : std::thread::hardware_concurrency())) {}

    void Submit(std::function<void()> task) override
    {
        pool_.detach_task(std::move(task));
    }
    std::size_t QueueDepth() const override { return pool_.get_tasks_queued(); }

private:
    BS::thread_pool pool_;
};

inline std::unique_ptr<IExecutor> make_default_executor(std::size_t threads)
{
    return std::make_unique<BSThreadPoolExecutor>(threads);
}

} // namespace detail
} // namespace uniflow
