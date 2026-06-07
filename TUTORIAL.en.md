# uniflow Tutorial

> 🌐 Language: **English** | [한국어](TUTORIAL.md)

One concept per chapter. Each chapter gives you a *complete, compilable* chunk of code. After the 9 chapters plus the final orchestration section, anything under [examples/](examples/) will read naturally.

> New here? Read the 3 quick tutorials in the [README](README.en.md) first (round-robin / async / observer). This document is the next step.

We'll pretend each chapter's code lives at `tutorial/chapNN.cpp`. To build:

```powershell
cl /std:c++17 /EHsc /I . tutorial\chap01.cpp /Fe:chap01.exe
```

Or with g++:
```bash
g++ -std=c++17 -O2 -pthread -I . tutorial/chap01.cpp -o chap01
```

> 💡 Every chapter starts with `#include "uniflow.hpp"`. No other dependencies.

---

## Chapter 1. A one-step module

The smallest uniflow module - one step function, one `Done()`. That's it.

```cpp
#include "uniflow.hpp"
#include <iostream>

class Hello : public uniflow::Uniflow<Hello> {
    UF_UNIFLOW_IMPLEMENT(Hello);
public:
    explicit Hello(uniflow::Runtime& rt)
        : uniflow::Uniflow<Hello>(rt) {}

    StepResult OnHello_Begin() {
        std::cout << "hello from a step\n";
        return Done();
    }
};

int main() {
    uniflow::Runtime rt;
    Hello h{rt};
    UF_START_FLOW(h, OnHello_Begin);
    h.WaitUntilIdle();
}
```

What the console shows:
```
[Hello         ] FLOW START  caller=chap01.cpp:18 main()
hello from a step
[Hello         ] (entry)      ...  #00 elapsed=0.05ms  tick x1 ...
[Hello         ] FLOW END  DONE  steps=#00 ...
```

**What just happened**
- `Runtime` spun up one pump thread.
- `Hello h{rt};` attached the module - the pump visits `h` every round from now on.
- `UF_START_FLOW(h, OnHello_Begin)` armed the flow. The step gets called on the next round.
- The step returned `Done()`, so the module goes back to idle.

**One thing to remember** - *never* call `std::this_thread::sleep_for` (or any other blocking call) inside a step body. The entire pump pauses for that long. Use `Stay()` (next chapter) or `UF_ASYNC` (chapter 6) instead.

---

## Chapter 2. A chain of steps (Next)

One step alone isn't useful. Real flows are N steps chained. Use `UF_NEXT(next_step)` to advance.

```cpp
class Greet : public uniflow::Uniflow<Greet> {
    UF_UNIFLOW_IMPLEMENT(Greet);
public:
    explicit Greet(uniflow::Runtime& rt)
        : uniflow::Uniflow<Greet>(rt) {}

    StepResult OnGreet_Begin() {
        std::cout << "1. hi there\n";
        return UF_NEXT(OnGreet_Middle);
    }

private:
    StepResult OnGreet_Middle() {
        std::cout << "2. nice to see you\n";
        return UF_NEXT(OnGreet_End);
    }
    StepResult OnGreet_End() {
        std::cout << "3. see you again\n";
        return Done();
    }
};

int main() {
    uniflow::Runtime rt;
    Greet g{rt};
    UF_START_FLOW(g, OnGreet_Begin);
    g.WaitUntilIdle();
}
```

**Why chop a flow into steps** - because step boundaries are exactly where you'll want to hook in a callback or an async result. When a step kicks off a 1-second job, it returns immediately with `UF_ASYNC`, and the *next* step picks up the result. The pump keeps running other modules in between.

**Entry steps are public** - outside callers (like `UF_START_FLOW`) need them. Middle steps are typically private, since they belong to this module's internal flow.

---

## Chapter 3. Polling with `Stay()`

Want to re-run the same step until some condition is met? Return `Stay()`. The pump sleeps for `stay_sleep_ms` (default 20 ms) and runs the step again.

```cpp
class WaitForFive : public uniflow::Uniflow<WaitForFive> {
    UF_UNIFLOW_IMPLEMENT(WaitForFive);
public:
    explicit WaitForFive(uniflow::Runtime& rt)
        : uniflow::Uniflow<WaitForFive>(rt) {}

    StepResult OnCount_Begin() {
        started_at_ = uniflow::Clock::now();
        return UF_NEXT(OnCount_Tick);
    }

private:
    StepResult OnCount_Tick() {
        auto elapsed = uniflow::Clock::now() - started_at_;
        if (elapsed >= std::chrono::seconds(5)) {
            std::cout << "5 seconds elapsed\n";
            return Done();
        }
        // not yet; come back next round
        return Stay();
    }

    uniflow::TimePoint started_at_;
};
```

**What `Stay()` is for** - polling a hardware flag, waiting for another module's state to change, "wait until condition true". For *actual blocking work* (network/disk/long compute), use `UF_ASYNC` instead.

---

## Chapter 4. `Describe` - a one-liner for debugging

When each step writes a short "what am I doing right now" to the observer/log, debugging gets a lot easier. `Describe(...)` produces that line.

```cpp
StepResult OnLoad_WaitAtSource() {
    if (GlobalEnv::Stop()) return Done();
    x_axis_.Update(GlobalGeometry::kXSpeed_mm_per_s);
    Describe("approaching A");          // <-- this line
    if (x_axis_.InPosition()) return UF_NEXT(OnLoad_CmdLowerToPick);
    return Stay();
}
```

The console line looks like:
```
[UF_LoadPicker ] OnLoad_WaitAtSource    approaching A    #01 elapsed=42ms  ...
```

`Describe` takes variadic args and stitches them with `<<`:
```cpp
Describe("parked at A-gap: stage=", ToString(stage_state),
         " partner_in_B=", partner_in_b);
```

The most recent description is printed once when the step transitions away, then cleared. Feel free to overwrite it on each step.

---

## Chapter 5. Many modules sharing one Runtime

Key fact: **modules attached to the same Runtime share the same pump thread**. That means shared state between modules needs no locks.

```cpp
namespace shared { std::ostringstream g_log; int g_turn = 0; }

class Pinger : public uniflow::Uniflow<Pinger> {
    UF_UNIFLOW_IMPLEMENT(Pinger);
public:
    StepResult OnPing_Begin() { return UF_NEXT(OnPing_Loop); }
private:
    StepResult OnPing_Loop() {
        if (count_ <= 0) return Done();
        if (shared::g_turn != 0) return Stay();  // wait my turn
        shared::g_log << "ping ";
        shared::g_turn = 1;
        --count_;
        return Stay();
    }
    int count_ = 5;
};

class Ponger : public uniflow::Uniflow<Ponger> {
    UF_UNIFLOW_IMPLEMENT(Ponger);
public:
    StepResult OnPong_Begin() { return UF_NEXT(OnPong_Loop); }
private:
    StepResult OnPong_Loop() {
        if (count_ <= 0) return Done();
        if (shared::g_turn != 1) return Stay();
        shared::g_log << "pong\n";
        shared::g_turn = 0;
        --count_;
        return Stay();
    }
    int count_ = 5;
};

int main() {
    uniflow::Runtime rt;
    Pinger p{rt};
    Ponger q{rt};
    UF_START_FLOW(p, OnPing_Begin);
    UF_START_FLOW(q, OnPong_Begin);
    p.WaitUntilIdle();
    q.WaitUntilIdle();
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

Neither `g_log` nor `g_turn` has a mutex - they're only touched from the same pump thread. ([Example 1: shared_ostream](examples/shared_ostream/) is the polished version of this pattern.)

**Warning** - if you create *two* Runtimes you get two pump threads. At that point shared state between modules on different Runtimes does need synchronisation. Usually one Runtime is plenty.

---

## Chapter 6. Real blocking work - `UF_ASYNC`

What if you call a 5-second HTTP request directly inside a step body? The pump freezes for 5 seconds, which means every other module freezes too. Don't do that.

The fix: `UF_ASYNC(static_fn, args...)` ships the call to the pool, and the *next step* picks up the result.

```cpp
class Fetcher : public uniflow::Uniflow<Fetcher> {
    UF_UNIFLOW_IMPLEMENT(Fetcher);
public:
    explicit Fetcher(uniflow::Runtime& rt)
        : uniflow::Uniflow<Fetcher>(rt) {}

    StepResult OnFetch_Begin(std::string url) {
        url_ = std::move(url);
        UF_ASYNC(DoHttpGet, url_);              // off to a pool thread
        return UF_NEXT(OnFetch_Done);            // receive in next step
    }

private:
    StepResult OnFetch_Done() {
        auto r = AsyncResult<std::string>();
        if (r.failed()) {
            std::cout << "fetch failed\n";
            return Fail();
        }
        std::cout << "got " << r.value().size() << " bytes\n";
        return Done();
    }

    // UF_ASYNC targets MUST be static. The compiler blocks you from
    // touching `this` to keep the cross-thread story clean.
    static std::string DoHttpGet(std::string url) {
        // ... real HTTP call happens here, on a pool thread ...
        return "<html>...</html>";
    }

    std::string url_;
};
```

**Why must it be static** - while the work runs on a pool thread, the module might be running other steps on the pump thread. Touching member variables from both would race. So static is enforced, and you pass the data the worker needs *by copy* (`url_` was copied into the argument).

Read the result with `AsyncResult<T>()` - only valid in the *follow-up step*. A single step can't fire two `UF_ASYNC`s in parallel (only one in-flight at a time per module).

**Observability** - it all shows up in the console automatically:
```
[Fetcher       ] (entry)                ASYNC SUBMIT  DoHttpGet
[Fetcher       ] (entry) -> OnFetch_Done ...
[Fetcher       ]                        ASYNC DONE    DoHttpGet  wait=120.4ms
[Fetcher       ] OnFetch_Done           got 1024 bytes  #01 elapsed=120.5ms
```

---

## Chapter 7. Timeouts / errors / handling failure

`UF_ASYNC_TIMEOUT(fn, dur, args...)` - async with a deadline.

```cpp
using namespace std::chrono_literals;

StepResult OnQuery_Begin() {
    UF_ASYNC_TIMEOUT(SlowApi, 2s, query_);   // 2 seconds or bust
    return UF_NEXT(OnQuery_After);
}

StepResult OnQuery_After() {
    auto r = AsyncResult<Response>();
    if (r.is_timeout()) {
        Describe("API didn't respond in 2s");
        return Fail();
    }
    if (r.failed()) {
        try { std::rethrow_exception(r.exception()); }
        catch (const std::exception& e) {
            Describe("API threw: ", e.what());
        }
        return Fail();
    }
    return UF_NEXT(OnQuery_Use, r.value());
}
```

**The three states of `AsyncRef<T>`**:
- `is_timeout()` true - deadline missed
- `failed()` true - worker threw an exception; `exception()` holds the ptr
- both false - `value()` is valid

**What if a step body throws an exception?** - by default, the whole process tears down (`std::terminate`). To survive instead, override `CatchStepExceptions()` on your module to return `true`:

```cpp
class SoftFail : public uniflow::Uniflow<SoftFail> {
    UF_UNIFLOW_IMPLEMENT(SoftFail);
public:
    bool CatchStepExceptions() const { return true; }
    // Now an exception in a step fires OnStepThrew, the flow ends with
    // Fail(), and the pump keeps running other modules.
};
```

---

## Chapter 8. Swapping the observer

The default observer (`ConsoleObserver`) prints step transitions, async start/end, slow-step alarms - nicely formatted to stdout. When you need something fancier - also write to a file, ship to a log server, emit metrics - derive from `IUniflowObserver` and plug it into the Runtime.

```cpp
class MyObserver : public uniflow::IUniflowObserver {
public:
    void OnStepChanged(std::string_view obj,
                       std::string_view prev_step, std::string_view next_step,
                       std::string_view description,
                       int step_ordinal, double elapsed_ms,
                       const uniflow::TickStats& step_ticks) override {
        // your own format, file mirror, ...
    }
    void OnFlowEnded(std::string_view obj, uniflow::StepAction action, ...) override {
        if (action == uniflow::StepAction::Fail) {
            // page on Slack ...
        }
    }
};

int main() {
    uniflow::Runtime::Opts opts;
    opts.observer = std::make_unique<MyObserver>();
    uniflow::Runtime rt(std::move(opts));
    // ... every module on this Runtime gets MyObserver
}
```

[examples/cnc_pickers/env_log_observer.h](examples/cnc_pickers/env_log_observer.h) is a real-world example that mirrors output to both stdout and a file.

**Available hooks**:
- `OnFlowStarted` - flow begins
- `OnStepChanged` - step transitioned (most frequent)
- `OnAsyncSubmitted` / `OnAsyncCompleted` - async start / end
- `OnSlowCpuStep` - a step held the pump longer than threshold
- `OnSlowAsync` - an async job stayed in flight longer than threshold
- `OnStepThrew` - a step threw an exception
- `OnFlowEnded` - flow ended (success or failure)

---

## Putting it all together - orchestration

The pattern for a module that decides *what other modules should do next*, in what order, is what we'll call an "orchestrator". Typically it's one flow (`On*_Tick`) that loops forever, and each round it looks at `peer.IsIdle()` and decides whether to fire `UF_START_FLOW` on it.

```cpp
class Scheduler : public uniflow::Uniflow<Scheduler> {
    UF_UNIFLOW_IMPLEMENT(Scheduler);
public:
    explicit Scheduler(uniflow::Runtime& rt)
        : uniflow::Uniflow<Scheduler>(rt) {}

    StepResult OnSched_Begin() { return UF_NEXT(OnSched_Tick); }

private:
    StepResult OnSched_Tick() {
        auto& worker = App::inst().worker;
        if (worker.IsIdle() && JobQueue::HasOne()) {
            UF_START_FLOW(worker, OnWorker_Begin, JobQueue::Pop());
        }
        if (GlobalEnv::Stop()) return Done();
        return Stay();
    }
};
```

This pattern shows up in [cnc_pickers' UF_Orchestrator](examples/cnc_pickers/uf_orchestrator.cpp), [queue_drain's UF_Sender](examples/queue_drain/uf_sender.cpp), [message_dispatch's UF_Professor/UF_Friend](examples/message_dispatch/uf_professor.cpp) - almost every *real* uniflow module has this skeleton.

---

## Next

- [EXAMPLES.en.md](EXAMPLES.en.md) - six working projects walked through
- [DESIGN.md](DESIGN.md) - design rationale (currently Korean)
- [uniflow.hpp](uniflow.hpp) - the header itself, with substantial comments

Issues and PRs welcome.
