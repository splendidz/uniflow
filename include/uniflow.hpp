// ============================================================================
//  uniflow.hpp — single-threaded, step-driven cooperative async framework
//
//  Public surface for derived modules (typical usage):
//
//      class MyModule : public Uniflow<MyModule> {
//          using S = MyModule;
//      public:
//          MyModule(UniflowRuntime& rt) : Uniflow(rt) {
//              UF_ENTRY("DoThing", OnThing_Begin);
//          }
//      private:
//          StepResult OnThing_Begin()      { return UF_NEXT(OnThing_Validate); }
//          StepResult OnThing_Validate()   {
//              if (!ok_) return Fail();
//              UF_ASYNC(DoHeavyWork, payload_);            // static fn, no `this`
//              return UF_NEXT(OnThing_WorkDone);
//          }
//          StepResult OnThing_WorkDone()   {
//              auto r = AsyncResult<WorkOutcome>();
//              if (r.is_timeout()) return Fail();
//              if (r.failed())     return Fail();
//              result_ = std::move(r.value());
//              return Done();
//          }
//          static WorkOutcome DoHeavyWork(Payload p);      // runs on pool
//      };
//
//  Bring your own thread pool (BS::thread_pool from bshoshany/thread-pool is
//  what the bundled BSThreadPoolExecutor adapts).  Single header, C++17.
// ============================================================================
#pragma once

#include <any>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <future>
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
#include <iostream>
#include <iomanip>

// BS::thread_pool — bshoshany/thread-pool. Include only when the bundled
// executor adapter is needed (so the framework can be reused with other pools).
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
//  Step result vocabulary
// ─────────────────────────────────────────────────────────────────────────────
enum class StepAction : uint8_t
{
    Stay,    // re-run the same step next tick (manual polling)
    Advance, // move to result.next_fn next tick
    Done,    // flow completed normally → idle
    Fail     // flow aborted → idle
};

inline const char* to_string(StepAction a)
{
    switch (a)
    {
    case StepAction::Stay:
        return "Stay";
    case StepAction::Advance:
        return "Advance";
    case StepAction::Done:
        return "Done";
    case StepAction::Fail:
        return "Fail";
    }
    return "?";
}

// ─────────────────────────────────────────────────────────────────────────────
//  Async support types
// ─────────────────────────────────────────────────────────────────────────────

// Per-submission knobs for one UF_ASYNC call. Passed by value; defaults give
// "no timeout, no slow warning, run on the pool named default".
struct AsyncOpts
{
    Duration                timeout = Duration::max(); // give-up deadline
    std::optional<Duration> slow_warn_after;           // warn-if-still-pending
    std::string             executor_name = "default"; // which pool to use
};

// The exception stored in the async result slot when a submission exceeds its
// AsyncOpts::timeout. The continuation step sees it via AsyncResult<T>().
struct AsyncTimeout : std::runtime_error
{
    AsyncTimeout(const char* job, Duration elapsed)
        : std::runtime_error(std::string("async timeout: ") + job),
          job_label(job), elapsed(elapsed) {}
    const char* job_label;
    Duration    elapsed;
};

// A lightweight, non-owning view of the most-recent async result, handed to
// the continuation step by AsyncResult<T>(). Exactly one of three states:
//   - failed()     : the worker threw            -> exception() holds it
//   - is_timeout() : the worker missed its deadline
//   - neither      : success                     -> value() is valid
// `_val` points into the owning module's result slot, so use it before the
// step returns; do not stash the AsyncRef itself.
template <class T>
struct AsyncRef
{
    T*                 _val;     // -> module's last_async_value_ (null on fail/timeout)
    std::exception_ptr _exc;     // set if the worker threw
    bool               _timeout; // set if the deadline was missed

    bool                      failed() const { return _exc != nullptr; }
    bool                      is_timeout() const { return _timeout; }
    T&                        value() const { return *_val; } // only valid when !failed && !timeout
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

// Runs the task synchronously on the calling thread.  Useful for unit tests:
// the whole flow becomes deterministic, no scheduling races.  NOT useful for
// the demo — it would block the pump thread inside UF_ASYNC and trip the
// SLOW CPU step warning on every submit.
class InlineExecutor : public IExecutor
{
public:
    void   Submit(std::function<void()> task) override { task(); }
    size_t QueueDepth() const override { return 0; }
};

// Minimal fixed-size worker pool built on std::thread + condvar queue.
// Bundled so the framework is usable out of the box without an external
// dependency; swap in BSThreadPoolExecutor / your own IExecutor in production.
class StdThreadPool : public IExecutor
{
public:
    explicit StdThreadPool(std::size_t threads = 0)
    {
        if (threads == 0)
        {
            threads = std::max<unsigned>(1u, std::thread::hardware_concurrency());
        }
        workers_.reserve(threads);
        for (std::size_t i = 0; i < threads; ++i)
        {
            workers_.emplace_back([this]
                                  { Worker(); });
        }
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
                cv_.wait(lk, [&]
                         { return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty())
                    return;
                task = std::move(queue_.front());
                queue_.pop_front();
            }
            try
            {
                task();
            }
            catch (...)
            { /* worker swallows; promise carries it */
            }
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
// vector, which is what makes "how far did it get before failing" answerable.
struct TraceEntry
{
    enum class Kind
    {
        StepTransition,
        AsyncCompletion
    };
    Kind        kind;           // which of the two fields below applies
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
    int         last_success_length = 0; // step count of the most recent success
    int         max_seen_length     = 0; // longest run ever (the "#M" denominator)
    std::size_t success_count       = 0;
    std::size_t fail_count          = 0;
};

// Every log line / metric the framework produces is funnelled through one of
// these virtual hooks — the framework itself never touches std::cout. Plug in
// your own logger by subclassing this and calling UniflowRuntime::SetObserver;
// override only the events you care about (all default to no-ops). The runtime
// installs ConsoleObserver (below) by default.
class IUniflowObserver
{
public:
    virtual ~IUniflowObserver() = default;

    virtual void OnFlowStarted(std::string_view obj, std::string_view flow) {}
    virtual void OnStepRan(std::string_view obj, std::string_view flow,
                           std::string_view step, int ordinal, Duration cpu) {}
    virtual void OnAsyncSubmitted(std::string_view obj, std::string_view step,
                                  std::string_view job) {}
    virtual void OnAsyncCompleted(std::string_view obj, std::string_view job,
                                  Duration wait, bool had_error, bool timed_out) {}
    virtual void OnSlowCpuStep(std::string_view obj, std::string_view step,
                               Duration cpu) {}
    virtual void OnSlowAsync(std::string_view obj, std::string_view job,
                             Duration wait_so_far) {}
    virtual void OnFlowEnded(std::string_view obj, std::string_view flow,
                             StepAction                     terminal_action,
                             int                            reached_ordinal,
                             const std::vector<TraceEntry>& trace,
                             Duration                       wall_clock,
                             Duration                       total_cpu,
                             Duration                       total_async_wait,
                             const FlowStats&               stats) {}
};

// Default observer — pretty prints to stdout.
class ConsoleObserver : public IUniflowObserver
{
public:
    void OnFlowStarted(std::string_view obj, std::string_view flow) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << obj << "] " << flow << " ENTER\n";
    }
    void OnStepRan(std::string_view obj, std::string_view /*flow*/,
                   std::string_view step, int ordinal, Duration cpu) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << obj << "]   #" << pad2(ordinal) << " "
                  << step << "  cpu=" << fmt_ms(cpu) << "\n";
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
        if (timed_out)
            std::cout << "  [TIMEOUT]";
        else if (had_error)
            std::cout << "  [ERROR]";
        else
            std::cout << "  [ok]";
        std::cout << "\n";
    }
    void OnSlowCpuStep(std::string_view obj, std::string_view step,
                       Duration cpu) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << obj << "]   [WARN] SLOW CPU step " << step
                  << " took " << fmt_ms(cpu) << " on main thread\n";
    }
    void OnSlowAsync(std::string_view obj, std::string_view job,
                     Duration wait_so_far) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << obj << "]   [WARN] SLOW ASYNC " << job
                  << " still pending after " << fmt_ms(wait_so_far) << "\n";
    }
    void OnFlowEnded(std::string_view obj, std::string_view flow,
                     StepAction terminal_action,
                     int        reached_ordinal,
                     const std::vector<TraceEntry>& /*trace*/,
                     Duration         wall_clock,
                     Duration         total_cpu,
                     Duration         total_async_wait,
                     const FlowStats& stats) override
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::cout << "[" << obj << "] " << flow
                  << (terminal_action == StepAction::Done ? " DONE" : " FAILED")
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
//  Forward decls
// ─────────────────────────────────────────────────────────────────────────────
class UniflowRuntime;

// Uniflow<Derived> is a template, so the runtime cannot hold a list of "all
// modules" directly — every module is a different type. IUniflowObject is the
// non-template base they all share, so the runtime can store and drive them
// uniformly through this handful of virtual calls.
class IUniflowObject
{
public:
    virtual ~IUniflowObject()                                 = default;
    virtual bool               IsIdle() const                 = 0; // no flow running?
    virtual void               ExecuteOnce(IUniflowObserver&) = 0; // advance one tick
    virtual const std::string& InstanceName() const           = 0; // label for logs
};

// ─────────────────────────────────────────────────────────────────────────────
//  Uniflow<Derived> — the CRTP base your modules inherit
//
//  CRTP = Curiously Recurring Template Pattern: a module inherits from the
//  base parameterised on *itself* — `class OrderModule : Uniflow<OrderModule>`.
//  That hands the base the derived type at compile time, so it can:
//    - type step functions as pointers-to-member of the *derived* class, and
//    - call them with no virtual dispatch (see ExecuteOnce's static_cast).
// ─────────────────────────────────────────────────────────────────────────────
template <class Derived>
class Uniflow : public IUniflowObject
{
public:
    // StepResult is used inside StepFn's type, so forward-declare it first,
    // then define it once StepFn exists.
    struct StepResult;

    // A step is just a member function of the derived module taking no args
    // and returning a StepResult. StepFn is a pointer to such a function;
    // the runtime stores one in curr_fn_ and calls it each tick.
    using StepFn = StepResult (Derived::*)();

    // What a step function returns: an intent, not a state change. The runtime
    // reads `action` and (for Advance) follows next_fn. next_name is the
    // step's identifier string, captured by UF_NEXT(#fn) purely for logging.
    struct StepResult
    {
        StepAction  action    = StepAction::Stay;
        StepFn      next_fn   = nullptr; // set only when action == Advance
        const char* next_name = nullptr; // step name for the trace/log
    };

    explicit Uniflow(UniflowRuntime& rt) : runtime_(rt) {}
    ~Uniflow() override = default;

    // ── IUniflowObject ──
    bool IsIdle() const override
    {
        std::lock_guard<std::mutex> lk(op_mu_);
        return curr_fn_ == nullptr;
    }
    const std::string& InstanceName() const override { return instance_name_; }
    void               ExecuteOnce(IUniflowObserver& obs) override;

    // Used by UniflowRuntime::Create.
    void SetInstanceName(std::string name) { instance_name_ = std::move(name); }

    // ── Public control surface ──
    // Start a previously-registered flow.  Returns false if busy or unknown.
    // Safe to call from any thread — serialized with the pump's ExecuteOnce
    // by the per-object mutex.  Any domain-state writes the caller does
    // *before* Start() are visible to step functions via the mutex's
    // happens-before edge.
    bool Start(const char* flow_name)
    {
        std::lock_guard<std::mutex> lk(op_mu_);
        if (curr_fn_ != nullptr)
            return false;
        auto it = flows_.find(flow_name);
        if (it == flows_.end())
            return false;
        curr_fn_         = it->second.fn;
        curr_step_name_  = it->second.entry_step_name;
        curr_flow_name_  = it->second.flow_name_storage.c_str();
        curr_ordinal_    = 0;
        flow_started_at_ = Clock::now();
        total_cpu_       = {};
        total_async_     = {};
        trace_.clear();
        // Observer notified on next ExecuteOnce so all logs flow through the
        // runtime's drain loop and stay ordered with step events.
        flow_started_pending_ = true;
        return true;
    }

protected:
    // ── Helpers your step functions return ──
    // These just build a StepResult; the runtime acts on it after the step.
    static StepResult Stay() { return {StepAction::Stay, nullptr, nullptr}; } // re-run me
    static StepResult Done() { return {StepAction::Done, nullptr, nullptr}; } // finish OK
    static StepResult Fail() { return {StepAction::Fail, nullptr, nullptr}; } // abort flow
    // Advance to `fn`. Not called directly — the UF_NEXT macro fills in both
    // the member-function pointer and its name string.
    static StepResult Next(StepFn fn, const char* name)
    {
        return {StepAction::Advance, fn, name};
    }

    // Register a named flow entry point.  Use UF_ENTRY macro.
    void DefineFlow(const char* flow_name, StepFn entry, const char* entry_step_name)
    {
        FlowReg r;
        r.flow_name_storage = flow_name;
        r.fn                = entry;
        r.entry_step_name   = entry_step_name;
        flows_.emplace(flow_name, std::move(r));
    }

    // Submit a static function to the executor.  Use UF_ASYNC / UF_ASYNC_OPT.
    template <auto Fn, class... Args>
    void SubmitAsync(const char* job_label, AsyncOpts opts, Args&&... args);

    // Access the result of the most-recent async submission.  Call from the
    // continuation step (the one named via UF_NEXT after UF_ASYNC).
    template <class T>
    AsyncRef<T> AsyncResult()
    {
        return AsyncRef<T>{std::any_cast<T>(&last_async_value_),
                           last_async_exception_,
                           last_async_was_timeout_};
    }

    UniflowRuntime& runtime() { return runtime_; }

private:
    // Reset back to the idle state once a flow reaches Done or Fail.
    void ClearFlow()
    {
        curr_fn_        = nullptr; // nullptr curr_fn_ == "idle" (see IsIdle)
        curr_step_name_ = nullptr;
        curr_flow_name_ = nullptr;
        curr_ordinal_   = 0;
        async_pending_  = false;
        slow_warned_    = false;
    }

    UniflowRuntime& runtime_;                     // the owning runtime
    std::string     instance_name_ = "<unnamed>"; // label shown in logs

    // The whole module is single-threaded *logically*, but Start() may be
    // called from another thread while the pump thread is in ExecuteOnce().
    // This mutex serialises those two. Mutable so the const IsIdle() can lock.
    mutable std::mutex op_mu_;

    // ── Current step: the live position within the running flow ──
    StepFn      curr_fn_              = nullptr; // step to run next tick; nullptr = idle
    const char* curr_step_name_       = nullptr; // its name (for logs)
    const char* curr_flow_name_       = nullptr; // name of the flow in progress
    int         curr_ordinal_         = 0;       // # of steps run so far this flow
    TimePoint   flow_started_at_      = {};      // for the wall-clock figure
    Duration    total_cpu_            = {};      // summed main-thread time of steps
    Duration    total_async_          = {};      // summed time spent waiting on pools
    bool        flow_started_pending_ = false;   // defer OnFlowStarted to next tick

    // ── Flow registry: name -> entry step. Filled by DefineFlow / UF_ENTRY. ──
    struct FlowReg
    {
        std::string flow_name_storage;         // owns the name string
        StepFn      fn              = nullptr; // entry step
        const char* entry_step_name = nullptr; // entry step's name
    };
    std::unordered_map<std::string, FlowReg> flows_;

    // ── Async slot: state of the one in-flight UF_ASYNC submission ──
    // (one slot per module; a step submits, the next step harvests)
    bool                  async_pending_ = false; // is a job in flight?
    bool                  slow_warned_   = false; // OnSlowAsync fired once?
    std::future<std::any> pending_fut_;           // worker -> result channel
    const char*           pending_job_label_ = nullptr;
    AsyncOpts             pending_opts_;         // timeout / pool / warn
    TimePoint             pending_submitted_at_; // for elapsed/timeout calc

    // ── Last async result: written when the job completes, read by the
    //    continuation step via AsyncResult<T>(). std::any type-erases the
    //    result so this slot does not need to know the worker's return type. ──
    std::any           last_async_value_;
    std::exception_ptr last_async_exception_;
    bool               last_async_was_timeout_ = false;

    // ── Trace + stats: the per-flow event log and cross-flow counters that
    //    get handed to OnFlowEnded. ──
    std::vector<TraceEntry> trace_;
    FlowStats               stats_;
};

// ─────────────────────────────────────────────────────────────────────────────
//  UniflowRuntime — owns objects, drives the pump loop, holds executors
// ─────────────────────────────────────────────────────────────────────────────
class UniflowRuntime
{
public:
    struct Config
    {
        // A step holding the main thread longer than this triggers
        // OnSlowCpuStep — the single-thread-protection alarm.
        Duration slow_cpu_threshold = std::chrono::milliseconds(1);
        // How long the pump sleeps when every module is idle (no busy-spin).
        Duration idle_sleep = std::chrono::milliseconds(1);
        Config()            = default;
    };

    UniflowRuntime() : UniflowRuntime(Config()) {}
    explicit UniflowRuntime(Config cfg) : cfg_(cfg)
    {
        observer_ = std::make_unique<ConsoleObserver>();
    }

    // Stops and joins the background pump thread, if RunInBackground was used.
    ~UniflowRuntime() { Stop(); }

    // Object ownership: Create<T>("name", ctor_args...)
    template <class T, class... Args>
    T* Create(std::string instance_name, Args&&... args)
    {
        auto obj = std::make_unique<T>(*this, std::forward<Args>(args)...);
        obj->SetInstanceName(std::move(instance_name));
        T* raw = obj.get();
        objects_.push_back(std::move(obj));
        return raw;
    }

    // Executors are named so per-call AsyncOpts{.executor_name=...} can pick.
    void RegisterExecutor(std::string name, std::shared_ptr<IExecutor> exec)
    {
        executors_[std::move(name)] = std::move(exec);
    }
    IExecutor& GetExecutor(const std::string& name)
    {
        auto it = executors_.find(name);
        if (it == executors_.end())
        {
            throw std::runtime_error("unknown executor: " + name);
        }
        return *it->second;
    }

    void SetObserver(std::unique_ptr<IUniflowObserver> obs)
    {
        observer_ = std::move(obs);
    }
    IUniflowObserver& Observer() { return *observer_; }

    const Config& config() const { return cfg_; }

    // The pump loop — the single thread that drives everything. Each round
    // ticks every non-idle module once (round-robin: no module can starve
    // another). If a round did real work, loop again immediately; if nothing
    // was active, sleep one idle interval so an idle runtime costs no CPU.
    // Blocks until RequestStop(); run it on its own thread (or use
    // RunInBackground). It does NOT clear the stop flag on entry — doing so
    // would race with a RequestStop() that arrives before this loop starts.
    void Run()
    {
        while (!stop_.load(std::memory_order_relaxed))
        {
            bool any_active = false;
            for (auto& obj : objects_)
            {
                if (obj->IsIdle())
                    continue;                 // skip modules with no flow
                obj->ExecuteOnce(*observer_); // advance this module one tick
                any_active = true;
            }
            if (!any_active)
                std::this_thread::sleep_for(cfg_.idle_sleep);
        }
    }
    // Ask Run() to return. Safe to call from any thread (atomic flag).
    void RequestStop() { stop_.store(true, std::memory_order_relaxed); }

    // Convenience: run the pump on a thread the runtime owns. Returns at once;
    // flows then execute in the background. End it with Stop() — or simply let
    // the runtime go out of scope, since the destructor calls Stop() for you.
    // Use plain Run() instead when you want to own the pump thread yourself
    // (e.g. to run the pump on the main thread).
    void RunInBackground()
    {
        if (pump_thread_.joinable())
            return; // already running
        // Clear the stop flag here, before the thread starts — never inside
        // Run(), or it would race with a Stop() that arrives first.
        stop_.store(false, std::memory_order_relaxed);
        pump_thread_ = std::thread([this]
                                   { Run(); });
    }

    // Stop the pump and join the owned thread. Idempotent, and also called by
    // ~UniflowRuntime. (The join is a no-op if you used plain Run() yourself.)
    void Stop()
    {
        RequestStop();
        if (pump_thread_.joinable())
            pump_thread_.join();
    }

private:
    Config                                                      cfg_;
    std::vector<std::unique_ptr<IUniflowObject>>                objects_;
    std::unordered_map<std::string, std::shared_ptr<IExecutor>> executors_;
    std::unique_ptr<IUniflowObserver>                           observer_;
    std::atomic<bool>                                           stop_{false};
    std::thread                                                 pump_thread_; // RunInBackground
};

// ─────────────────────────────────────────────────────────────────────────────
//  Uniflow<Derived> — out-of-line method definitions that need UniflowRuntime
//  (defined here, after UniflowRuntime is a complete type)
// ─────────────────────────────────────────────────────────────────────────────

// ExecuteOnce advances this module by *one tick*. The runtime calls it
// round-robin on every non-idle module. One tick does at most one of:
//   - nothing yet (an async job it is waiting on has not finished), or
//   - run exactly one step function and act on what that step returned.
// It never blocks: a not-ready async job makes it return immediately.
template <class Derived>
void Uniflow<Derived>::ExecuteOnce(IUniflowObserver& obs)
{
    // One lock guards the whole tick, pairing with Start()'s lock so an
    // external Start() can never interleave with a tick in progress.
    std::lock_guard<std::mutex> lk(op_mu_);

    // Start() only sets a flag; the actual OnFlowStarted log is emitted here
    // so it stays ordered with the step events the pump thread produces.
    if (flow_started_pending_)
    {
        obs.OnFlowStarted(instance_name_, curr_flow_name_ ? curr_flow_name_ : "");
        flow_started_pending_ = false;
    }
    if (curr_fn_ == nullptr)
        return; // idle — no flow running, nothing to do

    // ── 1. Gate on pending async (if any) ──
    // If the previous step submitted async work, do not run the continuation
    // step until that work is finished (or has timed out).
    if (async_pending_)
    {
        auto now     = Clock::now();
        auto elapsed = now - pending_submitted_at_;
        // wait_for(0) just polls — it never blocks the pump thread.
        bool ready     = pending_fut_.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
        bool timed_out = (elapsed > pending_opts_.timeout);

        // Still running and still within deadline: emit a one-time slow
        // warning if configured, then leave. Returning here is an implicit
        // Stay — the continuation step is simply not invoked this tick.
        if (!ready && !timed_out)
        {
            if (pending_opts_.slow_warn_after && elapsed > *pending_opts_.slow_warn_after && !slow_warned_)
            {
                obs.OnSlowAsync(instance_name_, pending_job_label_, elapsed);
                slow_warned_ = true;
            }
            return; // implicit Stay — user step not invoked
        }

        // The job is ready, or it blew its deadline → move its outcome into
        // the result slot so the continuation step can read it.
        if (timed_out && !ready)
        {
            // Timed out: store an AsyncTimeout exception in the slot so the
            // continuation's AsyncResult<T>().is_timeout() reports true.
            last_async_value_.reset();
            last_async_exception_ = std::make_exception_ptr(
                AsyncTimeout{pending_job_label_, elapsed});
            last_async_was_timeout_ = true;
        }
        else
        {
            // Finished: pull the value out of the future. future::get()
            // rethrows whatever the worker threw, so a throw becomes a
            // stored exception_ptr rather than crashing the pump thread.
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
        // CPU time so "main-thread cost" stays honest (see total_async_).
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
        // Slot is filled; fall through to run curr_fn_ — the continuation
        // step that UF_NEXT named right after the UF_ASYNC call.
    }

    // ── 2. Dispatch the step function (timed) ──
    // Call curr_fn_ on the derived object and measure how long it held the
    // main thread. The static_cast<Derived*> is the CRTP payoff: curr_fn_ is
    // a pointer-to-member of Derived, so this is a plain, non-virtual call.
    const char* step_name_at_call = curr_step_name_;
    auto        t0                = Clock::now();
    StepResult  r                 = (static_cast<Derived*>(this)->*curr_fn_)();
    auto        cpu_dt            = Clock::now() - t0;

    total_cpu_ += cpu_dt; // running main-thread cost for this flow
    curr_ordinal_++;      // this step gets the next ordinal number

    // Record the step in the trace (handed to OnFlowEnded at the end).
    {
        TraceEntry te;
        te.kind     = TraceEntry::Kind::StepTransition;
        te.ordinal  = curr_ordinal_;
        te.name     = step_name_at_call ? step_name_at_call : "";
        te.cpu_time = cpu_dt;
        te.action   = r.action;
        trace_.push_back(std::move(te));
    }

    obs.OnStepRan(instance_name_,
                  curr_flow_name_ ? curr_flow_name_ : "",
                  step_name_at_call ? step_name_at_call : "",
                  curr_ordinal_, cpu_dt);

    // A step is supposed to be quick (it shares the one thread with every
    // other module). If it overran the threshold, raise the core alarm.
    if (cpu_dt > runtime_.config().slow_cpu_threshold)
    {
        obs.OnSlowCpuStep(instance_name_,
                          step_name_at_call ? step_name_at_call : "",
                          cpu_dt);
    }

    // If this very step called UF_ASYNC, async_pending_ is now set — tell the
    // observer a job was submitted. The gate in section 1 will pick it up on
    // a later tick.
    if (async_pending_)
    {
        obs.OnAsyncSubmitted(instance_name_,
                             step_name_at_call ? step_name_at_call : "",
                             pending_job_label_ ? pending_job_label_ : "");
    }

    // ── 3. Apply the StepResult ──
    // The step expressed an intent; here the runtime acts on it.
    switch (r.action)
    {
    case StepAction::Stay:
        // Keep curr_fn_ as-is: the same step runs again next tick.
        break;
    case StepAction::Advance:
        // Move the cursor to the step UF_NEXT named.
        curr_fn_        = r.next_fn;
        curr_step_name_ = r.next_name;
        break;
    case StepAction::Done:
    case StepAction::Fail:
    {
        // Terminal: update cross-flow counters, hand the whole trace to the
        // observer, then reset to idle.
        if (r.action == StepAction::Done)
        {
            stats_.success_count++;
            stats_.last_success_length = curr_ordinal_;
            // max_seen_length feeds the "reached #N/#M" log — M is the
            // longest this flow has ever run.
            if (curr_ordinal_ > stats_.max_seen_length)
                stats_.max_seen_length = curr_ordinal_;
        }
        else
        {
            stats_.fail_count++;
        }
        obs.OnFlowEnded(instance_name_,
                        curr_flow_name_ ? curr_flow_name_ : "",
                        r.action, curr_ordinal_, trace_,
                        Clock::now() - flow_started_at_,
                        total_cpu_, total_async_, stats_);
        ClearFlow(); // back to idle; IsIdle() is true again
        break;
    }
    }
}

// SubmitAsync hands a job to a thread pool and arms the async slot. `Fn` is a
// non-type template parameter — the actual function, baked in at compile time
// (so the worker lambda can call it with zero indirection). UF_ASYNC fills it
// in as &S::SomeStaticFn.
template <class Derived>
template <auto Fn, class... Args>
void Uniflow<Derived>::SubmitAsync(const char* job_label,
                                   AsyncOpts   opts,
                                   Args&&... args)
{
    // Safety gate, checked at compile time: the target must NOT be a
    // non-static member function. A worker running on another thread must not
    // touch `this` — everything it needs is passed explicitly through args.
    static_assert(!std::is_member_function_pointer_v<decltype(Fn)>,
                  "UF_ASYNC target must be a `static` member or free function. "
                  "Instance state is unsafe across threads — pass needed data through args.");

    // R = the worker's return type, deduced from Fn and the argument types.
    using R = std::invoke_result_t<decltype(Fn), Args...>;

    // promise/future is the one-shot channel from the worker thread back to
    // the pump thread. shared_ptr because the lambda (on the pool) and this
    // module (holding pending_fut_) both need to outlive each other's view.
    auto promise          = std::make_shared<std::promise<std::any>>();
    pending_fut_          = promise->get_future();
    pending_job_label_    = job_label;
    pending_opts_         = std::move(opts);
    pending_submitted_at_ = Clock::now();
    async_pending_        = true; // ExecuteOnce section 1 now gates on this
    slow_warned_          = false;

    // Copy the arguments into a value tuple. std::decay_t strips references,
    // so even if the caller passed a member like `current_x_`, the worker
    // gets an independent copy — it can never reach back into `this`.
    auto tup = std::tuple<std::decay_t<Args>...>(std::forward<Args>(args)...);

    // Queue the job. The lambda runs on a pool thread: it unpacks the tuple
    // into Fn via std::apply, and routes the outcome — value or exception —
    // into the promise. The pump thread harvests it later in section 1.
    runtime_.GetExecutor(pending_opts_.executor_name).Submit([promise, tup = std::move(tup)]() mutable
                                                             {
            try {
                // is_void_v split: a void worker has no value to store, so
                // the slot gets an empty std::any as a "completed" marker.
                if constexpr (std::is_void_v<R>) {
                    std::apply([](auto&&... a) {
                        Fn(std::forward<decltype(a)>(a)...);
                    }, std::move(tup));
                    promise->set_value(std::any{});
                } else {
                    R result = std::apply([](auto&&... a) {
                        return Fn(std::forward<decltype(a)>(a)...);
                    }, std::move(tup));
                    promise->set_value(std::any(std::move(result)));
                }
            } catch (...) {
                // Any throw is captured and rethrown by future::get() on the
                // pump thread — the worker thread itself never crashes.
                promise->set_exception(std::current_exception());
            } });
}

} // namespace uniflow

// ─────────────────────────────────────────────────────────────────────────────
//  Convenience macros
//
//  Every macro below relies on `S` being an alias for the current class, so
//  each module must declare `using S = MyClass;` once. The macros exist to
//  capture two things you should not have to repeat by hand:
//    - `&S::fn`  — a pointer-to-member to the step / worker function, and
//    - `#fn`     — that function's name as a string, for the trace and logs.
//  Keeping the name next to the pointer is why step names appear in logs
//  without any manual logging in your code.
// ─────────────────────────────────────────────────────────────────────────────

// Put `UF_USES_UNIFLOW(MyClass);` once inside the class body. It grants the
// base template friendship so it can reach your *private* static UF_ASYNC
// targets — g++ checks that access at the base's instantiation context and
// would otherwise reject private helpers.
#define UF_USES_UNIFLOW(self) friend class ::uniflow::Uniflow<self>

// Advance to step `fn`: returns a StepResult carrying the pointer + name.
#define UF_NEXT(fn) this->Next(&S::fn, #fn)

// Register `fn` as the entry step of a named flow (call from the constructor).
#define UF_ENTRY(flow, fn) this->DefineFlow(flow, &S::fn, #fn)

// Submit static function `fn` to the "default" pool with default options.
// `, ##__VA_ARGS__` is the GNU comma-elision form: the leading comma is
// dropped when no extra args are passed, so UF_ASYNC(fn) on its own stays
// valid. GCC, Clang, and the MSVC preprocessor all honour it — no special
// compiler flag needed (unlike the C++20 __VA_OPT__ this previously used).
#define UF_ASYNC(fn, ...) this->template SubmitAsync<&S::fn>( \
    #fn, ::uniflow::AsyncOpts{}, ##__VA_ARGS__)

// Same as UF_ASYNC but with an explicit AsyncOpts (timeout / pool / warn).
#define UF_ASYNC_OPT(fn, opts, ...) this->template SubmitAsync<&S::fn>( \
    #fn, (opts), ##__VA_ARGS__)
