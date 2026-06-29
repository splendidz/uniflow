# uniflow Tutorial

> Language: **English** | [한국어](TUTORIAL.kr.md)

One concept per chapter. Each chapter provides a complete, compilable chunk of code written against the current API (uniflow `1.0.0`). After the chapters and the final orchestration section, the code under [examples/](examples/) reads naturally.

> If you are new here, start with the [README](../README.md). [Why this exists](../README.md#why-this-exists) describes the problem this framework was built to solve - the three ways a flow is usually written in C++ and how each one fails - and the philosophy that follows from it. This tutorial is the hands-on next step: it teaches the mechanics one at a time.

Structure: Chapters 1-4 cover the model itself - a module, a task, the first step, a chain of steps, polling, and the debug one-liner. Chapters 5-12 cover the surrounding machinery (shared state, blocking work, timeouts, observers, multi-runtime, virtual time, lifecycle). Chapter 13 returns to **task structure** - how a long flow is grouped into several compiler-enforced units, the structural backbone of every real flow.

Two ideas introduced in Chapter 1 and used throughout the rest:

- A **module** is a class deriving from `uniflow::Uniflow<Derived>`. One `Runtime` drives every attached module on a single pump thread.
- A module's logic lives in one or more **tasks**, each a `struct` deriving from `uniflow::Task<Flow>`. A task owns the state its steps share and the step functions themselves. A one-step flow is also a task. This is the baseline shape, not an advanced add-on, so flows are written as task-based from the first example.

The same example set ships in three languages: C++ here, Python under [python/examples](../python/examples), and C# under [cs/examples](../cs/examples). The names correspond across all three.

Assume each chapter's code lives at `tutorial/chapNN.cpp`. To build:

```powershell
cl /std:c++17 /EHsc /I . tutorial\chap01.cpp /Fe:chap01.exe
```

Or with g++:
```bash
g++ -std=c++17 -O2 -pthread -I . tutorial/chap01.cpp -o chap01
```

> Every chapter starts with `#include "uniflow.hpp"`. No other dependencies. The only macro required is `UF_FN(step)`, which passes a step function to `Next` / `StayTimeout` / `StayUntil` / `SubmitAsync` as a pointer-plus-label pair.

---

## Chapter 1. A one-step module

The minimal uniflow module: one task, one step, one `Done()`. A task overrides `Entry()` to name its first step; the step returns an intent.

```cpp
#include "uniflow.hpp"
#include <iostream>

class Flow_Hello : public uniflow::Uniflow<Flow_Hello>
{
public:
    explicit Flow_Hello(uniflow::Runtime& rt)
        : uniflow::Uniflow<Flow_Hello>(rt, "Flow_Hello")
    {
        AddTask(task_say_);
    }

    // The task is public so any thread (here, main) can launch it.
    struct Task_Say : uniflow::Task<Flow_Hello>
    {
        StepResult Entry() override { return Step1_Hello(); }

    private:
        StepResult Step1_Hello()
        {
            std::cout << "hello from a step\n";
            return Done();
        }
    } task_say_;
};

int main()
{
    uniflow::Runtime rt;
    Flow_Hello       hello{rt};
    hello.task_say_.StartFlow();   // launch the task
    hello.WaitUntilIdle();
}
```

What the console shows:
```
[Flow_Hello    ] FLOW START
hello from a step
[Flow_Hello    ] Step1_Hello             ...  #00 elapsed=0.05ms  tick x1 ...
[Flow_Hello    ] FLOW END  DONE  steps=#00 ...
```

**What happened**
- `Runtime rt;` started one pump thread.
- `Flow_Hello hello{rt};` attached the module; the pump visits `hello` every round from this point on. The constructor calls `AddTask(task_say_)` once, which wires the task's `flow()` back-pointer.
- `hello.task_say_.StartFlow()` armed the flow at the task's `Entry()` step. The step runs on the next round.
- `Entry()` returned `Step1_Hello()`, which printed and returned `Done()`, so the module returns to idle.

**The pieces, named once.** A *module* (`Flow_Hello`) is the durable object on the runtime. A *task* (`Task_Say`) is one unit of work it can run. A *step* (`Step1_Hello`) is one cooperative slice of that task. Steps are numbered (`Step1_`, `Step2_`, ...) and live as private members of their task; only the task itself is public, because launching it is the only operation the outside world performs.

**One rule to remember:** never call `std::this_thread::sleep_for` (or any other blocking call) inside a step body. The entire pump pauses for that duration. Use `Stay()` (Chapter 3) or `SubmitAsync` (Chapter 6) instead.

---

## Chapter 2. A chain of steps (Next)

A single step is rarely sufficient. Real tasks chain several steps. Return `Next(UF_FN(next_step))` to advance to a sibling step of the same task on the next pump round.

```cpp
#include "uniflow.hpp"
#include <iostream>

class Flow_Greet : public uniflow::Uniflow<Flow_Greet>
{
public:
    explicit Flow_Greet(uniflow::Runtime& rt)
        : uniflow::Uniflow<Flow_Greet>(rt, "Flow_Greet")
    {
        AddTask(task_greet_);
    }

    struct Task_Greet : uniflow::Task<Flow_Greet>
    {
        StepResult Entry() override { return Step1_Open(); }

    private:
        StepResult Step1_Open()
        {
            std::cout << "1. hi there\n";
            return Next(UF_FN(Step2_Middle));
        }
        StepResult Step2_Middle()
        {
            std::cout << "2. nice to see you\n";
            return Next(UF_FN(Step3_Close));
        }
        StepResult Step3_Close()
        {
            std::cout << "3. see you again\n";
            return Done();
        }
    } task_greet_;
};

int main()
{
    uniflow::Runtime rt;
    Flow_Greet       greet{rt};
    greet.task_greet_.StartFlow();
    greet.WaitUntilIdle();
}
```

**Why a flow is divided into steps:** step boundaries are where a callback or an async result is introduced. When a step starts a one-second job, it returns immediately with `SubmitAsync` and the next step reads the result. The pump runs other modules in between.

**`Next` never leaves the task.** It can only name another step of this task; the compiler enforces this, because `UF_FN(fn)` resolves `fn` against the current task type. Crossing to another task is a separate operation (`StartTask`, Chapter 13); a flow cannot silently fall from one unit into the next.

**Passing data along the chain.** A step may take parameters; pass them after the name and they are carried into the next step:

```cpp
return Next(UF_FN(Step2_Use), payload);   // Step2_Use(Payload p)
```

The value is bound by copy into the next-step thunk, so it outlives the current step body. The common use is carrying an `AsyncId` to the step that reads its result (Chapter 6).

---

## Chapter 3. Polling with `Stay()`

To re-run the same step until a condition is met, return `Stay()`. The pump sleeps for `stay_sleep_ms` (default 20 ms) and runs the step again.

```cpp
#include "uniflow.hpp"
#include <iostream>

class Flow_Counter : public uniflow::Uniflow<Flow_Counter>
{
public:
    explicit Flow_Counter(uniflow::Runtime& rt)
        : uniflow::Uniflow<Flow_Counter>(rt, "Flow_Counter")
    {
        AddTask(task_count_);
    }

    struct Task_Count : uniflow::Task<Flow_Counter>
    {
        // OnEnter runs once each time the task is entered, before the first
        // step - the place to (re-)arm a per-run timer.
        void OnEnter() override { t_.Restart(); }
        StepResult Entry() override { return Step1_Tick(); }

    private:
        uniflow::UFTimer t_;

        StepResult Step1_Tick()
        {
            using namespace std::chrono_literals;
            if (t_.Passed(5000ms))
            {
                std::cout << "5 seconds elapsed\n";
                return Done();
            }
            return Stay();   // not yet - come back next round
        }
    } task_count_;
};

int main()
{
    uniflow::Runtime rt;
    Flow_Counter     counter{rt};
    counter.task_count_.StartFlow();
    counter.WaitUntilIdle();
}
```

**Uses of `Stay()`:** polling a hardware flag, waiting for another module's state to change, waiting until a condition is true. For actual blocking work (network / disk / long compute), use `SubmitAsync` instead (Chapter 6).

### `UFTimer` - measuring time inside a step

In a single-threaded cooperative model, `sleep` cannot be used, since it would freeze the whole pump. Every "wait N ms", "time out after T", or "settle for D" therefore becomes a timer polled across `Stay()` rounds. This is how time works in this model.

A `uniflow::UFTimer` is a stopwatch with three reads:

```cpp
uniflow::UFTimer t;        // armed at construction; re-arm any time with t.Restart()

t.Elapsed();               // duration since it was armed
t.Passed(2000ms);          // bool: has 2s passed since arming?
t.HeldFor(cond, 50ms);     // bool: has 'cond' been continuously true for 50ms?
```

- **`Passed(d)`** - has the given duration elapsed since arming? This is the 5-second wait above.

- **`HeldFor(cond, d)`** - settling / debounce. True only once `cond` has held true for `d` continuously; a single false reading restarts the count. For a hardware flag that bounces before it is stable: `if (t.HeldFor(Hw::Ready(), 50ms)) ...`.

- **`Elapsed()`** - the raw duration, used for pacing or progress: `double frac = to_ms(t.Elapsed()) / total_ms;`.

**Where to keep it.** A timer that spans steps (armed in step A, checked in step B) must outlive the step. Make it a **member of the task** and re-arm it in `OnEnter()`, as with `t_` above. Because the timer belongs to the task, it survives every `Stay()` re-entry and is reset at the next task entry, with no manual bookkeeping.

**Built-in per-step timer.** Every module also has a built-in timer reached from a step via `flow().StepTimer()`. Unlike the per-task `TaskBase::Elapsed()` (reset on task entry), it is re-armed on **every step change** - a `Next`, a `StayUntil` timeout, a task switch, or flow start - but not on a `Stay`, so it measures time within the current step with no member to declare. To give your own member timers the same auto-reset, create them with `flow().NewAutoTimer()` instead of a plain `UFTimer`; the module re-arms every registered timer on each step change. A `UFTimer` you reset yourself is unaffected.

> **One clock that can be scaled or frozen.** Bind a timer to the runtime - `uniflow::UFTimer t{rt.clock()}` - and it follows that runtime's virtual clock. `rt.clock().SetScale(10)` plays the whole flow back at 10x; `rt.clock().Freeze()` / `.Resume()` holds every logical timeout in place (for example during an e-stop, so a 3s timeout does not fire while the line is paused). A plain `UFTimer{}` uses real time. Async / IO deadlines always remain on real time, regardless of scale. Chapter 10 covers this.

To poll a condition but **route to a recovery step if it never arrives**, see `StayTimeout` / `StayUntil` in Chapter 7.

---

## Chapter 4. `Describe` - a one-liner for debugging

When each step writes a short description of its current activity to the observer / log, debugging is easier. `Describe(...)` produces that line.

```cpp
StepResult Step3_WaitInPos()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    flow().axis_->Move(target_mm_);
    Describe("approaching target");      // <-- this line
    if (flow().axis_->InPosition())
    {
        return Next(UF_FN(Step4_Clamp));
    }
    return Stay();
}
```

The console line looks like:
```
[Flow_Mover    ] Step3_WaitInPos        approaching target    #01 elapsed=42ms  ...
```

`Describe` takes variadic args and stitches them with `<<`:
```cpp
Describe("parked at A-gap: stage=", ToString(stage_state),
         " partner_in_B=", partner_in_b);
```

The most recent description is printed once when the step transitions away, then cleared. It may be overwritten on each step. (A step reaches the module's own state through `flow()`, a typed reference to the parent module with access to its private members. Chapter 5 covers this.)

---

## Chapter 5. Many modules sharing one Runtime

The defining property: **modules attached to the same Runtime share the same pump thread**. Shared state between modules therefore needs no locks.

```cpp
#include "uniflow.hpp"
#include <iostream>
#include <sstream>

namespace shared
{
std::ostringstream g_log;
int                g_turn = 0;
}

class Flow_Ping : public uniflow::Uniflow<Flow_Ping>
{
public:
    explicit Flow_Ping(uniflow::Runtime& rt)
        : uniflow::Uniflow<Flow_Ping>(rt, "Flow_Ping")
    {
        AddTask(task_ping_);
    }

    struct Task_Ping : uniflow::Task<Flow_Ping>
    {
        StepResult Entry() override { return Step1_Loop(); }

    private:
        int count_ = 5;

        StepResult Step1_Loop()
        {
            if (count_ <= 0)
            {
                return Done();
            }
            if (shared::g_turn != 0)
            {
                return Stay();   // wait my turn
            }
            shared::g_log << "ping ";
            shared::g_turn = 1;
            --count_;
            return Stay();
        }
    } task_ping_;
};

class Flow_Pong : public uniflow::Uniflow<Flow_Pong>
{
public:
    explicit Flow_Pong(uniflow::Runtime& rt)
        : uniflow::Uniflow<Flow_Pong>(rt, "Flow_Pong")
    {
        AddTask(task_pong_);
    }

    struct Task_Pong : uniflow::Task<Flow_Pong>
    {
        StepResult Entry() override { return Step1_Loop(); }

    private:
        int count_ = 5;

        StepResult Step1_Loop()
        {
            if (count_ <= 0)
            {
                return Done();
            }
            if (shared::g_turn != 1)
            {
                return Stay();
            }
            shared::g_log << "pong\n";
            shared::g_turn = 0;
            --count_;
            return Stay();
        }
    } task_pong_;
};

int main()
{
    uniflow::Runtime rt;
    Flow_Ping        ping{rt};
    Flow_Pong        pong{rt};
    ping.task_ping_.StartFlow();
    pong.task_pong_.StartFlow();
    ping.WaitUntilIdle();
    pong.WaitUntilIdle();
    std::cout << shared::g_log.str();
}
```

Output:
```
ping pong
ping pong
ping pong
ping pong
ping pong
```

Neither `g_log` nor `g_turn` has a mutex; both are accessed only from the same pump thread.

**Reaching a module's own state: `flow()`.** A step is a member of its task, not of the module, so `this` is the task. The owning module, where the durable hardware / peer state lives, is reached through `flow()`, a typed reference wired by `AddTask`. Because the task is a nested type of the module, `flow()` can read the module's private members as well: `flow().axis_->Move(...)`, `flow().PartnerInZoneB()`. A sibling task's state is reached the same way: `flow().task_other_`.

**Note:** creating two Runtimes produces two pump threads. At that point shared state between modules on different Runtimes requires synchronisation, and the lock-free ways to bridge them (`Post` / `Link`) are covered in Chapter 9. One Runtime is usually sufficient.

---

## Chapter 6. Real blocking work - `SubmitAsync`

Calling a 5-second HTTP request directly inside a step body freezes the pump for 5 seconds, which freezes every other module as well. This must be avoided.

The solution: `SubmitAsync(UF_FN(static_fn), timeout, args...)` ships the call to the thread pool and returns an `AsyncId`. Carry that id to a later step, and poll the result there with `AsyncResult<T>(id)`.

```cpp
#include "uniflow.hpp"
#include <iostream>
#include <string>

class Flow_Fetcher : public uniflow::Uniflow<Flow_Fetcher>
{
public:
    explicit Flow_Fetcher(uniflow::Runtime& rt)
        : uniflow::Uniflow<Flow_Fetcher>(rt, "Flow_Fetcher")
    {
        AddTask(task_fetch_);
    }

    struct Task_Fetch : uniflow::Task<Flow_Fetcher>
    {
        StepResult Entry() override { return Step1_Begin(); }

    private:
        StepResult Step1_Begin()
        {
            // No deadline -> Duration::max(). The id identifies this job.
            uniflow::AsyncId id =
                SubmitAsync(UF_FN(DoHttpGet), uniflow::Duration::max(),
                            std::string("http://example.com"));
            if (id == 0)
            {
                return Fail();   // submission rejected (in-flight cap)
            }
            return Next(UF_FN(Step2_Wait), id);   // carry the id forward
        }

        StepResult Step2_Wait(uniflow::AsyncId id)
        {
            auto r = AsyncResult<std::string>(id);
            if (r.pending())
            {
                return Stay();   // worker still in flight - poll again
            }
            if (!r.ok())
            {
                std::cout << "fetch failed\n";
                return Fail();
            }
            std::cout << "got " << r.return_value->size() << " bytes\n";
            return Done();
        }

        // The async target MUST be static (a free function works too). It runs
        // on a pool thread, so it cannot touch task / module state - pass
        // everything it needs by value.
        static std::string DoHttpGet(std::string url)
        {
            (void)url;
            // ... real HTTP call happens here, on a pool thread ...
            return "<html>...</html>";
        }
    } task_fetch_;
};

int main()
{
    uniflow::Runtime rt;
    Flow_Fetcher     fetcher{rt};
    fetcher.task_fetch_.StartFlow();
    fetcher.WaitUntilIdle();
}
```

**Why it must be static:** while the work runs on a pool thread, the module may be running other steps on the pump thread. Accessing member variables from both would race. Static is therefore enforced, and the data the worker needs is passed by copy (the URL string is copied into the argument).

**Read the result by polling.** `AsyncResult<T>(id)` returns an `AsyncOutcome<T>` by value; check it every round:
- `r.pending()` - still in flight; `Stay()` and poll again.
- `r.ok()` - the worker returned; `*r.return_value` (a `std::optional<T>`) holds it.
- `r.failed()` / `r.is_timeout()` - the worker threw or missed its deadline.
- `r.found()` - false only for a bad / cleared / `0` id.

Several jobs can be in flight at once, each with its own id. `AnyAsyncPending()` is true while any is unresolved; `JoinAllAsync(UF_FN(then))` re-`Stay()`s until all resolve, then advances; `ClearAsync()` abandons every slot.

**Observability:** these events are reported to the console automatically:
```
[Flow_Fetcher  ] Step1_Begin             ASYNC SUBMIT  DoHttpGet
[Flow_Fetcher  ] Step1_Begin -> Step2_Wait ...
[Flow_Fetcher  ]                        ASYNC DONE    DoHttpGet  wait=120.4ms
[Flow_Fetcher  ] Step2_Wait              got 1024 bytes  #01 elapsed=120.5ms
```

---

## Chapter 7. Timeouts / errors / handling failure

Two kinds of timeout, two roles. An **async job** can overrun its deadline; a **polled step** can wait indefinitely for a flag that never arrives. uniflow provides one tool for each.

### Async deadline - the `SubmitAsync` timeout

The second argument to `SubmitAsync` is the deadline. Pass an actual `Duration` instead of `Duration::max()`, and a worker that misses it resolves as `TimedOut`:

```cpp
using namespace std::chrono_literals;

StepResult Step1_Query()
{
    uniflow::AsyncId id = SubmitAsync(UF_FN(SlowApi), 2000ms, query_);   // 2s or bust
    if (id == 0)
    {
        return Fail();
    }
    return Next(UF_FN(Step2_After), id);
}

StepResult Step2_After(uniflow::AsyncId id)
{
    auto r = AsyncResult<Response>(id);
    if (r.pending())
    {
        return Stay();
    }
    if (r.is_timeout())
    {
        Describe("API did not respond in 2s");
        return Fail();
    }
    if (r.failed())
    {
        Describe("API worker threw");
        return Fail();
    }
    return Next(UF_FN(Step3_Use), *r.return_value);
}
```

`AsyncOutcome<T>` classifies the slot - `is_timeout()`, `failed()`, `ok()`, `pending()` - so a result that is not present is never dereferenced. (There is no `.value()` or `.exception()`; the value lives in `return_value`, engaged only when `ok()`.)

### Step deadline - `StayTimeout`, the step-level `catch`

Often the step is not waiting on a job at all: it commanded some hardware and is now `Stay()`-polling a "done" flag, a sensor, or a peer module's state. If the condition never arrives - a jammed axis, a lost encoder, a stuck valve - a bare `Stay()` loop polls indefinitely. The line then hangs with no error, which on a real machine is among the worst outcomes.

`StayTimeout(dur, UF_FN(fn))` is a `Stay()` with a deadline: keep polling this step, but if `dur` elapses since the step was entered, route to step `fn` instead. `fn` is the `catch`, a guaranteed exit to a known recovery path. (The body still owns the happy path - it returns `Next`/`Done` itself; the deadline only guarantees an exit if the wait never resolves.)

The full pattern is command, wait-with-deadline, recover:

```cpp
using namespace std::chrono_literals;

// Move an axis to a target, then wait for it to report InPosition.
StepResult Step1_Command()
{
    flow().axis_->Move(target_mm_);              // fire the command (non-blocking)
    Describe("moving to ", target_mm_, " mm");
    return Next(UF_FN(Step2_WaitInPos));
}

StepResult Step2_WaitInPos()
{
    if (flow().axis_->InPosition())              // the happy path
    {
        return Next(UF_FN(Step3_Clamp));
    }
    // still moving - keep polling, but give up if it stalls past 2s
    return StayTimeout(2000ms, UF_FN(Step_Stalled));
}

// Reached ONLY if InPosition never became true within 2s of entering the
// wait step. The flow cannot hang - it always lands somewhere defined.
StepResult Step_Stalled()
{
    flow().axis_->Abort();                       // stop the motion
    Describe("axis stalled before reaching target");
    return Fail();
}
```

Without `StayTimeout`, `Step2_WaitInPos` returning a bare `Stay()` would spin until someone notices the line is dead. With it, the flow is guaranteed to reach `Step_Stalled` if the move does not complete; the flow always makes forward progress to a defined state.

**The recovery step is an ordinary step, so it can route anywhere.** A common shape is retry, then give up:

```cpp
StepResult Step2_WaitInPos()
{
    if (flow().axis_->InPosition())
    {
        return Next(UF_FN(Step3_Clamp));
    }
    return StayTimeout(2000ms, UF_FN(Step_Retry));
}

StepResult Step_Retry()
{
    if (++attempts_ >= 3)                          // out of retries
    {
        Describe("axis failed to reach target after 3 tries");
        return Fail();
    }
    flow().axis_->Abort();
    Describe("retry ", attempts_, "/3");
    return Next(UF_FN(Step1_Command));             // re-issue -> re-enters the
}                                                  // wait, restarting the 2s window
```

(`attempts_` is a member of the task, reset in `OnEnter()`.) Re-entering `Step1_Command` -> `Step2_WaitInPos` is a fresh step entry, so the 2s deadline restarts for each attempt, with no manual timer bookkeeping.

Three points worth noting:

- **The deadline is measured from step entry, not from the `StayTimeout` call.** It is returned on every poll tick, but the clock does not restart each tick; a step that polls for 2s times out at 2s.
- **It is logical time.** The deadline runs on the runtime's clock (`rt.clock()` from Chapter 3), so `Freeze()` holds it - a 2s timeout does not fire while the line is e-stopped - and `SetScale` scales it. The `SubmitAsync` deadline stays on real time.
- **Two timeouts, two roles.** The `SubmitAsync` timeout means "this job must finish within T" (read `is_timeout()` when polling). `StayTimeout` means "this step must make progress within T" (the flow lands in a recovery step). Use the first for async work, the second for polled conditions.

It pairs with `HeldFor` (Chapter 3): require the flag to be stable, but still route out if it never settles:

```cpp
StepResult Step2_WaitReady()
{
    if (settle_.HeldFor(flow().hw_ready_->IsReady(), 50ms))   // ready AND settled 50ms
    {
        return Next(UF_FN(Step3_Done));
    }
    return StayTimeout(3000ms, UF_FN(Step_Timeout));          // never settled -> recover
}
// settle_ is a UFTimer member of the task, re-armed in OnEnter().
```

This wait-with-settle pairing is common enough that **`StayUntil`** folds it into one call: a wait condition, a settle duration, and both targets. `condition` is polled each round; once it has stayed true for `settle`, the step routes to the success target, but if `timeout` elapses first it routes to the timeout target instead. The argument order is `condition, settle, success, timeout, timeout_step` (the same across the Python / C# ports). The whole `Step2_WaitReady` above becomes one line:

```cpp
StepResult Step2_WaitReady()
{
    return StayUntil([this] { return flow().hw_ready_->IsReady(); }, 50ms,
                     UF_FN(Step3_Done), 3000ms, UF_FN(Step_Timeout));
}
```

The settle window here is tracked by the framework (no member timer needed), reset on each step entry like the built-in timer below.

**If a step body throws an exception:** by default, the whole process terminates (`std::terminate`). To continue instead, override `CatchStepExceptions()` on the module to return `true`:

```cpp
class Flow_SoftFail : public uniflow::Uniflow<Flow_SoftFail>
{
public:
    explicit Flow_SoftFail(uniflow::Runtime& rt)
        : uniflow::Uniflow<Flow_SoftFail>(rt, "Flow_SoftFail") {}

    // Now an exception in a step fires OnStepThrew, the flow ends with
    // Fail(), and the pump keeps running other modules.
    bool CatchStepExceptions() const { return true; }
};
```

---

## Chapter 8. Swapping the observer

The default observer (`ConsoleObserver`) prints step transitions, async start / end, and slow-step alarms, formatted to stdout. For additional behavior - writing to a file, sending to a log server, emitting metrics - derive from `IUniflowObserver` and install it on the Runtime.

```cpp
class MyObserver : public uniflow::IUniflowObserver
{
public:
    void OnStepChanged(std::string_view obj,
                       std::string_view prev_step, std::string_view next_step,
                       std::string_view description,
                       int step_ordinal, double elapsed_ms,
                       const uniflow::TickStats& step_ticks) override
    {
        // your own format, file mirror, ...
    }

    void OnFlowEnded(std::string_view obj, uniflow::StepAction terminal_action,
                     int final_step_ordinal,
                     const std::vector<uniflow::TraceEntry>& trace,
                     double wall_ms, double total_step_ms, double total_async_ms,
                     const uniflow::FlowTickSummary& flow_ticks,
                     const uniflow::FlowStats& stats,
                     uniflow::FlowOrigin origin) override
    {
        if (terminal_action == uniflow::StepAction::Fail)
        {
            // page on Slack ...
        }
    }
};

int main()
{
    uniflow::Runtime::Opts opts;
    opts.observer = std::make_unique<MyObserver>();
    uniflow::Runtime rt{std::move(opts)};
    // ... every module on this Runtime reports through MyObserver
}
```

An **empty** subclass is the silent observer: all hooks are no-ops by default, so a console app that owns its own screen can suppress framework output entirely:

```cpp
struct SilentObserver : uniflow::IUniflowObserver {};
```

[examples/pick_and_place/env_log_observer.h](examples/pick_and_place/env_log_observer.h) is a real-world example that mirrors output to both stdout and a file; the [simulator](examples/simulator/) uses the silent pattern above.

**Available hooks** (all optional):
- `OnFlowStarted` - a task was armed.
- `OnStepChanged` - a step transitioned (most frequent).
- `OnAsyncSubmitted` / `OnAsyncCompleted` - async start / end.
- `OnSlowCpuStep` - a step held the pump longer than threshold.
- `OnSlowAsync` - an async job stayed in flight longer than threshold.
- `OnAsyncAbandoned` / `OnAsyncHighWater` - a worker outlived its flow / the in-flight cap was hit.
- `OnStepThrew` - a step threw.
- `OnFlowEnded` - a flow ended (success or failure).
- `OnPostSubmitted` / `OnPostExecuted` / `OnLinked` - cross-runtime traffic (Chapter 9).
- `OnSlowRound` - a pump round overran (below).

### Tracking slow cycles - round profiling

Step / flow stats describe one module; sometimes the relevant question is why one pump round took 50ms. A round drains posted callbacks and runs every active module once. Round profiling measures this.

```cpp
uniflow::Runtime::Opts o;
o.config.slow_round_threshold_ms = std::chrono::milliseconds(20); // alarm past 20ms
o.config.trace_rounds            = true;                          // + per-step/post breakdown
uniflow::Runtime rt{std::move(o)};
```

- **Round timing stats** - `rt.GetRoundStats()` -> `{count, min_ms, max_ms, avg_ms, last_ms}` (work rounds only; idle polling excluded).
- **Reset the peak** - `max_ms` is the peak; clear it with `rt.ResetRoundStats()`.
- **Heavy trace on / off** - `rt.SetRoundTracing(true/false)`. On fills the per-segment breakdown below; off still provides the low-cost round length. Toggle at runtime.
- **Slow-cycle alarm** - past the threshold, `OnSlowRound(runtime_index, profile)` fires:

```cpp
void OnSlowRound(int rt_index, const uniflow::RoundProfile& p) override
{
    // p.busy_ms     : total work time this round
    // p.segments[i] : { kind(Step/Post), obj, label, ms }  <- names the culprit
    for (const auto& s : p.segments) { /* the longest ms is the culprit */ }
}
```

What the default `ConsoleObserver` prints:
```
[rt#0          ] [SLOW ROUND]  busy=52.10ms  segments=2
                 Step  Flow_Stage     Step1_Process                48.30ms
                 Post  rt#0           net.cpp:88 OnPoll()           3.80ms
```

---

## Chapter 9. Between Runtimes - `Post` and `Link`

Chapter 5 noted that two Runtimes means two pump threads, and shared state between them needs a lock. Using a lock discards this framework's lock-free premise. Instead, uniflow provides two ways to **funnel the access onto one pump thread**.

### `Post` - throw a callback to another pump

When another runtime (or a plain thread, or non-uniflow code) needs to access state a runtime owns, do not access it directly; `Post` a callback to that runtime. The callback runs on that runtime's pump thread, so no lock is needed.

```cpp
uniflow::Runtime net_rt;
// ... network modules attached to net_rt ...

// from the main thread (or another runtime):
net_rt.Post([] {
    // this lambda runs on net_rt's pump thread - no lock
    ConnectionTable::MarkAllStale();
});
```

This is the same pattern as libuv's `uv_async_send` or Qt's `invokeMethod(..., Qt::QueuedConnection)`. The pump drains and runs queued callbacks at the top of each round, then wakes itself so the callback is serviced this round, not after the next sleep.

> For richer logging, `PostAt(caller, fn)` tags the call with its source location (file / line / function) so `OnPostSubmitted` / `OnPostExecuted` can report where the callback originated. Bare `Post` posts with a blank call site.

**Rule:** a posted callback is a raw callback running outside the step / flow model (no trace). It must therefore be short and non-blocking. Holding the pump too long stalls that whole runtime. For blocking work, start a flow from inside the callback with `task.StartFlow()`.

### `PostAndWait` - when you need a value back

To read a value back, use `PostAndWait`. The callback runs on the pump thread and the calling thread waits for the result (`std::future`).

```cpp
std::future<int> f = net_rt.PostAndWait([] {
    return ConnectionTable::Count();   // read safely on net_rt's pump
});
int count = f.get();                   // calling thread blocks here
```

**Never** call `PostAndWait` from the pump thread that drives the target runtime: the pump that must run the callback is the one blocked waiting for the result, so it never unblocks (deadlock). Do not call it from inside a step; an assert catches this case.

### `Link` - fuse two pumps into one

When sharing is frequent enough that `Post` becomes cumbersome, fuse the two runtimes onto one pump thread. After `driver.Link(other)`:

- `other`'s pump thread stops, and
- `driver`'s pump runs `other`'s modules every round too, and
- `other` keeps its own observer / executor / config / module list (it only lends out the pump thread).

```cpp
uniflow::Runtime rt;
uniflow::Runtime sub_rt;

Flow_Something m{sub_rt};   // module belongs to sub_rt
rt.Link(sub_rt);           // but rt's pump drives m

m.task_run_.StartFlow();    // runs on rt's pump
```

Once fused, `rt`'s modules and `sub_rt`'s modules are serialized on one thread, so shared state between them needs no lock either. Each module keeps its own slow thresholds / observer / executor; only the pump's sleep cadence follows the driver's `Config`. `LinkAt` captures the call site for the `OnLinked` observer callback.

**`Link` is one-way:** once fused, the runtimes cannot be split back. After fusing, flows on the two sides may have developed dependencies on each other, so it is no longer determinable which module is safe to move back to which pump. Therefore:

> **Recommended default: start with one Runtime.** Multiple pumps are an optimization applied deliberately, only when independence is certain and the parallelism is genuinely needed. If the question "what if I need to share later?" arises, that indicates independence is not certain.

### Logging is on by default

All three operations flow through the observer. Control flow that hops pump threads is harder to debug, so the default `ConsoleObserver` also prints the caller site:

```
[rt#0          ] POST SUBMIT                  caller=net_worker.cpp:42 PollLoop()
[rt#0          ] POST RUN                     queue=0.67ms  caller=net_worker.cpp:42 PollLoop()
[rt#0          ] LINK                         rt#1 -> rt#0  caller=app.cpp:18 App::Start()
```

### When to use what

- Sharing is **occasional** -> `Post` / `PostAndWait` (localized, one resource owned by one runtime).
- Sharing is **frequent on the hot path** -> `Link` (fuse the two pumps).
- Three or more -> one driver can `Link` several (flat linking).

---

## Chapter 10. Virtual time - speed, freeze, e-stop

Every `UFTimer` and every `StayTimeout` / `StayUntil` deadline reads its time from the runtime's virtual clock, not directly from the wall clock. By default it tracks real time 1:1, but it can be scaled or frozen, and every logical timer on that runtime moves with it.

```cpp
rt.clock().SetScale(10.0);   // logical time runs 10x faster
rt.clock().SetScale(0.25);   // ... or 4x slower
rt.clock().Freeze();         // logical time stops
rt.clock().Resume();         // ... and continues from where it froze
```

This provides two capabilities:

- **Simulation playback.** A flow with `Passed(5000ms)` waits and `StayTimeout(3000ms, ...)` timeouts runs at the configured rate: drive a day-long line cycle in seconds for testing, or slow it down to observe. The [simulator](examples/simulator/) example is this case: five runners and a renderer share one clock, and a single `SetScale` / `Freeze` rescales or pauses the whole field at once.
- **A correct pause.** During an e-stop / hold, a 3-second "hw ready" timeout should not fire merely because the line sat still for 10 seconds. `Freeze()` stops every logical deadline; `Resume()` continues from where it left off, so timeouts measure running time, not wall time.

**What scales and what does not.** The virtual clock governs logical waits only - `UFTimer` (`Elapsed` / `Passed` / `HeldFor`) and `StayTimeout` / `StayUntil`. It deliberately does **not** affect the `SubmitAsync` deadline or the pump's own sleep: a real network call does not finish faster because the sim was scaled, so those stay on the real wall clock.

**Binding.** A timer built as `uniflow::UFTimer{rt.clock()}` follows that runtime's clock. A plain `uniflow::UFTimer{}` uses real time, which applies when a wall-clock measurement that ignores scale / freeze is required. To read virtual time directly, use `rt.clock().Now()`.

> Changing scale or freezing is continuous; the clock rebases on each change, so `Elapsed()` does not jump at the moment `SetScale` / `Freeze` is called.

---

## Chapter 11. Driving a flow from the outside - event threads and `Wake`

Real systems receive events from threads other than the pump: a socket receiving a message, a device-driver callback, a GUI button. Such an event should start (or feed) a flow immediately, not on the next 20ms poll.

**Launching a task from another thread is safe.** `StartFlow` / `StartTask` take the module's lock internally, so they can be called from any thread:

```cpp
// on some I/O thread:
void OnNetworkMessage()
{
    App::inst().handler.task_handle_.StartFlow();
}
```

One consideration: the pump may be sleeping (`stay_sleep_ms`, default 20ms) when the event arrives, so the first step could run up to 20ms late. `StartFlow` / `Post` therefore **wake the pump**: they bring it out of its sleep so the newly-armed flow runs on the next round. It can also be called explicitly:

```cpp
rt.Wake();   // pump out of its nap now, not at the next poll tick
```

`Wake()` is called directly when module state was mutated through a separate channel (not via a flow) and must be serviced now. The idiomatic way to access another runtime's state from outside is to `Post` a callback (Chapter 9), and `Post` already wakes the pump:

```cpp
net_rt.Post([&] { App::inst().handler.Enqueue(m); });   // runs on the pump, wakes it
```

> **Do not** access a module's members directly from an outside thread; that races with the pump. Launch a task, or `Post` a callback; both run on the pump thread and both wake it. `Wake()` is the low-level wake primitive the two build on.

This also applies when a `SubmitAsync` job finishes: it wakes the pump, so the polling step reads the result immediately instead of up to a poll-interval late.

---

## Chapter 12. Controlling a flow's lifecycle - idle, wait, stop

A module is either *idle* (no task running) or *busy* (one task running; a module runs one task at a time). Three calls manage that lifecycle.

- **`IsIdle()`** - is the module free? An orchestrator checks this before launching a task on a peer. `StartFlow` itself returns `StartResult::Busy` and does nothing if a task is already running, so the guard reads clearly:

```cpp
if (worker.IsIdle())
{
    worker.task_run_.StartFlow();
}
```

- **`WaitUntilIdle()`** - block the calling thread until the running task finishes. This is how `main()` waits for work to drain before exiting. Call it from the owning thread, never from inside a step (a step blocking on its own pump would deadlock):

```cpp
pipe.task_fetch_.StartFlow();
pipe.WaitUntilIdle();          // main thread parks here until the task ends
```

**Stopping a running flow is cooperative.** There is no `Cancel()` that removes a task mid-step; that would risk tearing down state a step is in the middle of using. Instead a flow ends when a step chooses to: it checks a stop signal and returns `Done()` or `Fail()`. Every long-running task uses this pattern:

```cpp
StepResult Step1_Tick()
{
    if (GlobalEnv::Stop())           // your own flag, set from anywhere
    {
        // ... release anything this task holds ...
        return Done();
    }
    // ... normal work ...
    return Stay();
}
```

Because the check is a step reading a flag, the developer decides where it is safe to stop: between motions, not mid-motion. For an orderly shutdown of many modules, set the flag, wake the pump, then `WaitUntilIdle()` each one:

```cpp
GlobalEnv::RequestStop();
rt.Wake();
for (auto* m : all_modules)
{
    m->WaitUntilIdle();
}
```

---

## Chapter 13. Many tasks per module - grouping a flow into units

Chapters 1-12 each used a module with a single task. That is the right shape for a short flow. But a real equipment sequence is fifteen, twenty, or thirty steps, and a flat chain of thirty steps, however well named, no longer answers the question that matters during maintenance: **which operation does this step belong to, where does that operation begin, and where does it end?**

The mechanism is already available: a module may hold several tasks, each a `struct Task_X : uniflow::Task<Flow>`. A **task** is a named unit operation - a group of steps that together perform one meaningful action (a *Pick*, a *Place*, a *Prepare*) - and the grouping is not a naming convention. It is enforced by the compiler, because a step is a member of its task's type.

### Declaring several units

Each task is a public member struct. The module's constructor calls `AddTask` once per task; durable hardware / machine state lives on the module (reached via `flow()`), while transient per-run state (a settle timer, a retry counter) lives on the task.

```cpp
class Flow_Loader : public uniflow::Uniflow<Flow_Loader>
{
public:
    explicit Flow_Loader(uniflow::Runtime& rt)
        : uniflow::Uniflow<Flow_Loader>(rt, "Flow_Loader")
    {
        AddTask(task_pick_);
        AddTask(task_place_);
    }

    // Unit: Pick - approach the source, grip, lift, then hand off to Place.
    struct Task_Pick : uniflow::Task<Flow_Loader>
    {
        StepResult Entry() override { return Step1_MoveToSource(); }

    private:
        StepResult Step1_MoveToSource();
        StepResult Step2_WaitAtSource();
        StepResult Step3_Grip();
        StepResult Step4_Lift();
    } task_pick_;

    // Unit: Place - carry to the destination and release.
    struct Task_Place : uniflow::Task<Flow_Loader>
    {
        StepResult Entry() override { return Step1_MoveToDest(); }

    private:
        StepResult Step1_MoveToDest();
        StepResult Step2_WaitAtDest();
        StepResult Step3_Release();
    } task_place_;

private:
    Motion* axis_;   // durable hardware, reached as flow().axis_
};
```

Step bodies are defined out of line, qualified by their task, and return `uniflow::StepResult` (bring it into scope with `using namespace uniflow;` in the .cpp to drop the prefix):

```cpp
using namespace uniflow;

// Advance WITHIN the unit with Next.
StepResult Flow_Loader::Task_Pick::Step1_MoveToSource()
{
    Describe("moving to source");
    flow().axis_->Move(kSourceX);
    return Next(UF_FN(Step2_WaitAtSource));
}

// Cross a unit boundary ONLY with StartTask.
StepResult Flow_Loader::Task_Pick::Step4_Lift()
{
    if (flow().axis_->AtPickHeight())
    {
        return StartTask(flow().task_place_);   // Pick done -> enter Place
    }
    return Stay();
}
```

### What the compiler now enforces

This is the purpose of the construct - invariants that no longer have to be maintained by discipline:

1. **A step's membership is fixed by its type.** `Task_Pick::Step1_MoveToSource` is a member of `Task_Pick`. Reading any one step identifies its unit; it cannot be renamed into the wrong group.
2. **Unit boundaries are explicit.** Inside `Task_Pick`, `Next(UF_FN(...))` can only name another `Task_Pick` step, because `UF_FN` resolves the name against the current task type. Naming a `Task_Place` step is a type mismatch and does not compile. The only way out of a unit is `StartTask`, so a flow cannot silently fall from one operation into the next.
3. **Entry is a contract.** `StartTask(flow().task_place_)` lands on `Task_Place::Entry()` and nowhere else. A unit's internal steps are private and cannot be entered from outside, so a unit's internals can be rearranged without touching any caller.

### Two ways to sequence units

`StartTask` from inside a step is the in-flow switch: the running task ends and the next begins on the same module, on the next pump round. It is the right tool when one unit always flows into the next.

More commonly, each task ends with `Done()` and an **orchestrator** decides what runs next, based on machine state. This keeps a unit free of any knowledge of what follows it. This is the dominant pattern in the reference examples (see the orchestration section below), and it is why `StartTask` is comparatively rare.

### Per-unit transient state

State that belongs to one run of a unit - a settle timer, a retry counter, a measured value - becomes a plain member of the task. Override `OnEnter()` to reset it when the unit is entered; it then survives the `Stay()` re-entries within the unit and is reset at the next entry:

```cpp
struct Task_Prepare : uniflow::Task<Flow_Stage>
{
    void OnEnter() override { settle_.Restart(); }   // reset per-run timer on entry
    StepResult Entry() override { return Step1_SendStart(); }

private:
    uniflow::UFTimer settle_;       // per-run settle timer

    StepResult Step1_SendStart();
    StepResult Step2_WaitReady();
    StepResult Step_Timeout();
} task_prepare_;
```

```cpp
StepResult Flow_Stage::Task_Prepare::Step2_WaitReady()
{
    using namespace std::chrono_literals;
    if (settle_.HeldFor(flow().hw_ready_->IsReady(), 50ms))   // settled
    {
        flow().state_ = StageState::Prepared;
        return Done();                                        // orchestrator runs Process next
    }
    return StayTimeout(3000ms, UF_FN(Step_Timeout));          // never settled -> recover
}
```

Compare this to threading a struct by hand through every step: the unit owns its working state, and the framework resets it at the boundary.

### Small flows don't pay for this

A three-step poller does not need three units. It is one task with one `Entry()`, exactly what Chapters 1-12 used. The model is uniform: a one-step flow and a thirty-step, three-task flow are written the same way, so the structure is present when a flow grows and adds little overhead when it does not.

> The full, running version of everything in this chapter is [pick_and_place](examples/pick_and_place/): two pickers as `Pick -> Place` task pairs, a Stage as `Prepare -> Process -> Cleanup`, with per-unit timers, async commands, and a `StayTimeout` hardware timeout. It is the reference read for multi-task flows.

---

## Putting it all together - orchestration

A module that decides what other modules should do next, and in what order, is termed an "orchestrator". Typically it is one perpetual task whose single step loops continuously; each round it examines `peer.IsIdle()` and the machine state and decides whether to launch a task on a peer.

```cpp
class Flow_Orchestrator : public uniflow::Uniflow<Flow_Orchestrator>
{
public:
    explicit Flow_Orchestrator(uniflow::Runtime& rt)
        : uniflow::Uniflow<Flow_Orchestrator>(rt, "Flow_Orchestrator")
    {
        AddTask(task_schedule_);
    }

    struct Task_Schedule : uniflow::Task<Flow_Orchestrator>
    {
        StepResult Entry() override { return Step1_Tick(); }

    private:
        StepResult Step1_Tick();
    } task_schedule_;
};
```

```cpp
using namespace uniflow;

StepResult Flow_Orchestrator::Task_Schedule::Step1_Tick()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }

    // Drive the stage: one task per machining phase, launched as the previous
    // phase completes. The stage never sequences itself.
    Flow_Stage& stage = App::inst().stage;
    if (stage.IsIdle())
    {
        switch (stage.state())
        {
        case StageState::RawPartLoaded:
            stage.task_prepare_.StartFlow();
            break;
        case StageState::Prepared:
            stage.task_process_.StartFlow();
            break;
        case StageState::Machined:
            stage.task_cleanup_.StartFlow();
            break;
        default:
            break;
        }
    }

    return Stay();   // poll the line forever
}
```

Each task ends with `Done()` and advances the module's state; the orchestrator's single looping step reads that state and launches the next task when the module is idle. The pickers and the stage do not decide their own sequence; the orchestrator owns it.

This is the structure of almost every real uniflow program: [pick_and_place's Flow_Orchestrator](examples/pick_and_place/uf_orchestrator.cpp) (a single `Schedule` task driving two pickers and a stage), and the spawner tasks in [message_dispatch](examples/message_dispatch/) that launch a consumer when their mailbox is non-empty.

---

## Next

- [EXAMPLES.md](EXAMPLES.md) - the working projects walked through.
- [uniflow.hpp](uniflow.hpp) - the header itself, with detailed comments. `uniflow::kVersion` is `"1.0.0"`.
- Same examples in [Python](../python/examples) and [C#](../cs/examples).

Issues and PRs welcome.
