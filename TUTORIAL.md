# uniflow Tutorial

> 🌐 Language: **English** | [한국어](TUTORIAL.kr.md)

One concept per chapter. Each chapter gives you a *complete, compilable* chunk of code. After the 12 chapters plus the final orchestration section, anything under [cpp/examples/](cpp/examples/) will read naturally.

> New here? Read the 3 quick tutorials in the [README](README.md) first (round-robin / async / observer). This document is the next step.

We'll pretend each chapter's code lives at `tutorial/chapNN.cpp`. To build:

```powershell
cl /std:c++17 /EHsc /I cpp tutorial\chap01.cpp /Fe:chap01.exe
```

Or with g++:
```bash
g++ -std=c++17 -O2 -pthread -I cpp tutorial/chap01.cpp -o chap01
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

### `UFTimer` - measuring time inside a step

Notice the example above hand-rolled a stopwatch: a `TimePoint` member plus `Clock::now()` math. That pattern is so common it has a helper. And it matters more than it looks: **in a single-threaded cooperative world you can never `sleep`** - that would freeze the whole pump - so *every* "wait N ms", "time out after T", or "settle for D" becomes a timer you poll across `Stay()` rounds. This is simply how time works here; get comfortable with it.

A `uniflow::UFTimer` is a stopwatch with three reads:

```cpp
uniflow::UFTimer t;        // armed at construction; re-arm any time with t.Restart()

t.Elapsed();               // Duration since it was armed
t.Passed(2s);            // bool: has 2s passed since arming?
t.HeldFor(cond, 50ms);      // bool: has 'cond' been continuously true for 50ms?
```

- **`Passed(d)`** - has the given duration elapsed since arming? The 5-second wait above collapses to:

```cpp
StepResult OnCount_Tick() {
    if (t_.Passed(5s)) { std::cout << "5 seconds elapsed\n"; return Done(); }
    return Stay();                       // not yet - poll again next round
}
// member: uniflow::UFTimer t_;  (re-armed in OnCount_Begin with t_.Restart())
```

- **`HeldFor(cond, d)`** - *settling* / debounce. True only once `cond` has held true for `d` straight; a single false reading restarts the count. For a hardware flag that bounces before it is really stable: `if (t_.HeldFor(Hw::Ready(), 50ms)) ...`.

- **`Elapsed()`** - the raw duration, handy for pacing or progress: `double frac = Elapsed() / total;`.

**Where to keep it.** A timer that spans steps (armed in step A, checked in step B) must outlive the step - make it a **member**, or pass one through `UF_NEXT(NextStep, uniflow::UFTimer{})` so a single timer is shared across a `Stay()` loop. A fresh local timer only measures within one step body.

> **One clock you can scale or freeze.** Bind a timer to the runtime - `uniflow::UFTimer t{rt.clock()}` - and it follows that runtime's *virtual* clock. `rt.clock().SetScale(10)` plays the whole flow back 10x; `rt.clock().Freeze()` / `.Resume()` holds every logical timeout in place (e.g. during an e-stop, so a 3s timeout does not fire while the line is paused). A plain `UFTimer{}` uses real time. Async/IO deadlines always stay on real time, regardless of scale.

To poll a condition but **bail to a recovery step if it never arrives**, see `UF_STAY_UNTIL` in Chapter 7.

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

Neither `g_log` nor `g_turn` has a mutex - they're only touched from the same pump thread. ([Example 1: shared_ostream](cpp/examples/shared_ostream/) is the polished version of this pattern.)

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

### Step timeouts: `UF_STAY_UNTIL` - the step-level `catch`

`UF_ASYNC_TIMEOUT` covers an async *job* that overran. But often you are not waiting on a job at all - you are `Stay()`-polling a flag, a peer module's state, or a hardware line that has no completion callback. What if it never comes? A bare `Stay()` loop would poll forever.

`UF_STAY_UNTIL(dur, fn)` is a `Stay()` with a deadline. The step keeps polling exactly like `Stay()`, but if `dur` elapses *since the step was entered*, the flow jumps to step `fn` instead. Think of `fn` as the step's `catch` / cleanup branch:

```cpp
StepResult OnArm_WaitReady() {
    if (Hw::Ready()) return UF_NEXT(OnArm_Move);   // got it - proceed
    return UF_STAY_UNTIL(3s, OnArm_Recover);       // still waiting; give up after 3s
}

// Reached only if Hw::Ready() never came true within 3s of entering the step
// above. Do whatever the situation needs, then end the flow.
StepResult OnArm_Recover() {
    Hw::AbortMotion();
    Describe("hw never reported ready - aborted");
    return Fail();                                 // or retry / reset / alarm ...
}
```

Three things worth knowing:

- **The deadline is measured from step entry, not from the `UF_STAY_UNTIL` call.** You return it on every poll tick, but the clock does not restart each tick - so a step that polls for 3s really times out at 3s, not never.
- **It is logical time.** The deadline runs on the runtime's clock (`rt.clock()` from Chapter 3), so `Freeze()` holds it - a 3s timeout will *not* fire while the line is e-stopped - and `SetScale` scales it. Async/IO deadlines stay on real time.
- **Two timeouts, two jobs.** `UF_ASYNC_TIMEOUT` = "this *job* must finish within T" (you read `is_timeout()` in the next step). `UF_STAY_UNTIL` = "this *step* must make progress within T" (you land in a recovery step). Use the first for `UF_ASYNC`, the second for polled conditions.

It pairs naturally with `HeldFor` from Chapter 3 - poll until a flag is *stable*, but bail if it never settles:

```cpp
StepResult OnArm_WaitReady(uniflow::UFTimer& t) {   // t carried via UF_NEXT(..., uniflow::UFTimer{})
    if (t.HeldFor(Hw::Ready(), 50ms)) return UF_NEXT(OnArm_Move);  // stable -> go
    return UF_STAY_UNTIL(3s, OnArm_Recover);                       // never settled -> recover
}
```

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

[cpp/examples/cnc_pickers/env_log_observer.h](cpp/examples/cnc_pickers/env_log_observer.h) is a real-world example that mirrors output to both stdout and a file.

**Available hooks**:
- `OnFlowStarted` - flow begins
- `OnStepChanged` - step transitioned (most frequent)
- `OnAsyncSubmitted` / `OnAsyncCompleted` - async start / end
- `OnSlowCpuStep` - a step held the pump longer than threshold
- `OnSlowAsync` - an async job stayed in flight longer than threshold
- `OnStepThrew` - a step threw an exception
- `OnFlowEnded` - flow ended (success or failure)

### Tracking slow cycles - round profiling

Step/flow stats look at *one module*; sometimes what you want is *"why did one pump round suddenly take 50ms?"*. A round = drain posted callbacks + run every active module once. Round profiling answers that.

```cpp
uniflow::Runtime::Opts o;
o.config.slow_round_threshold_ms = std::chrono::milliseconds(20); // alarm past 20ms
o.config.trace_rounds            = true;                          // + per-step/post breakdown
uniflow::Runtime rt(std::move(o));
```

- **Round timing stats** - `rt.GetRoundStats()` -> `{count, min_ms, max_ms, avg_ms, last_ms}` (work rounds only; idle polling excluded).
- **Reset the peak** - `max_ms` is the peak; clear it with `rt.ResetRoundStats()`.
- **Heavy trace on/off** - `rt.SetRoundTracing(true/false)`. On fills the per-segment breakdown below; off still gives the cheap round length (`busy_ms`). Toggle at runtime.
- **Slow-cycle alarm** - past the threshold, `OnSlowRound(runtime_index, profile)` fires:

```cpp
void OnSlowRound(int rt_index, const uniflow::RoundProfile& p) override {
    // p.busy_ms     : total work time this round
    // p.segments[i] : { kind(Step/Post), obj, label, ms }  <- names the culprit
    for (const auto& s : p.segments) { /* the longest ms is the culprit */ }
}
```

What the default `ConsoleObserver` prints:
```
[rt#0          ] [SLOW ROUND]  busy=52.10ms  segments=2
                 Step  Stage          OnProcess_WaitHwReady        48.30ms
                 Post  rt#0           net.cpp:88 OnPoll()           3.80ms
```

---

## Chapter 9. Between Runtimes - `Post` and `Link`

Chapter 5 noted that two Runtimes means two pump threads, and shared state between them needs a lock. But reaching for a lock throws away this framework's whole lock-free premise. Instead uniflow gives you two ways to **funnel the access onto one pump thread**.

### `Post` - throw a callback to another pump

When another runtime (or a plain thread, or non-uniflow code) wants to touch state a runtime owns, do not touch it directly - `UF_POST` a callback to that runtime. The callback runs on that runtime's pump thread, so no lock is needed.

```cpp
uniflow::Runtime net_rt;
// ... network modules attached to net_rt ...

// from the main thread (or another runtime):
UF_POST(net_rt, [] {
    // this lambda runs on net_rt's pump thread - no lock
    ConnectionTable::MarkAllStale();
});
```

This is the same pattern as libuv's `uv_async_send` or Qt's `invokeMethod(..., Qt::QueuedConnection)`. The pump drains and runs queued callbacks at the top of each round.

The `UF_POST` macro tags the call with its source location (file/line/function) for observer logging. You can call `net_rt.Post([]{...})` bare, but then the caller column in the log is blank. A callback hopping pump threads is invisible in a stack trace, so prefer the macro form to keep "where was this thrown from?" in the logs.

**Rule** - a posted callback is a raw callback running *outside* the step/flow model (no trace). So it must be *short and non-blocking*. Hold the pump too long and that whole runtime stalls. For blocking work, start a flow from inside the callback with `UF_START_FLOW`.

### `PostAndWait` - when you need a value back

To read something back, use `UF_POST_WAIT`. The callback runs on the pump thread and the calling thread waits for the result (`std::future`).

```cpp
std::future<int> f = UF_POST_WAIT(net_rt, [] {
    return ConnectionTable::Count();   // read safely on net_rt's pump
});
int count = f.get();                   // calling thread blocks here
```

**Never** call `UF_POST_WAIT` from the pump thread that drives the target runtime: the pump that must run the callback is the one blocked waiting for the result, so it never unblocks (deadlock). Do not call it from inside a step - an assert catches exactly this.

### `Link` - fuse two pumps into one

When sharing is so frequent that `Post` is tedious, fuse the two runtimes onto one pump thread. After `driver.Link(other)`:

- `other`'s pump thread stops, and
- `driver`'s pump runs `other`'s modules every round too, and
- `other` keeps its own observer / executor / config / module list (it only lends out the pump thread).

```cpp
uniflow::Runtime rt;
uniflow::Runtime sub_rt;

SomeModule m{sub_rt};                  // module belongs to sub_rt
UF_LINK(rt, sub_rt);                   // but rt's pump drives m

UF_START_FLOW(m, OnSomething_Begin);   // runs on rt's pump
```

Once fused, `rt`'s modules and `sub_rt`'s modules are serialized on one thread, so shared state between them needs no lock either. Each module keeps its own slow thresholds / observer / executor; only the pump's sleep cadence follows the driver's `Config`. `UF_LINK` also captures the call site for the `OnLinked` observer callback.

**`Link` is one-way** - once fused you cannot split them back. After fusing, flows on the two sides may have grown dependencies on each other, so it is no longer knowable which module is safe to move back to which pump. Hence:

> **Recommended default: start with one Runtime.** Multiple pumps are an optimization you reach for deliberately, only when independence is certain *and* you genuinely need the parallelism. If "what if I need to share later?" even crosses your mind, that is the signal that independence is not certain.

### Logging is on by default

All three flow through the observer. Control flow that hops pump threads is awkward to debug, so the default `ConsoleObserver` even prints the caller site:

```
[rt#0          ] POST SUBMIT                  caller=net_worker.cpp:42 PollLoop()
[rt#0          ] POST RUN                     queue=0.67ms  caller=net_worker.cpp:42 PollLoop()
[rt#0          ] LINK                         rt#1 -> rt#0  caller=app.cpp:18 App::Start()
```

- `OnPostSubmitted` - the moment of posting (on the calling thread, with caller)
- `OnPostExecuted` - the moment the pump actually ran it (`queue=` is the time it waited in the queue)
- `OnLinked` - the moment a link took effect

### When to use what

- Sharing is **occasional** -> `UF_POST` / `UF_POST_WAIT` (localized, one resource owned by one runtime)
- Sharing is **frequent on the hot path** -> `UF_LINK` (fuse the two pumps)
- Three or more -> one driver can `UF_LINK` several (flat linking)

---

## Chapter 10. Virtual time - speed, freeze, e-stop

Every `UFTimer` and every `UF_STAY_UNTIL` deadline reads its time from the runtime's *virtual clock*, not directly from the wall clock. By default it tracks real time 1:1, but you can scale or freeze it - and every logical timer on that runtime moves with it.

```cpp
rt.clock().SetScale(10.0);   // logical time runs 10x faster
rt.clock().SetScale(0.25);   // ... or 4x slower
rt.clock().Freeze();         // logical time stops
rt.clock().Resume();         // ... and continues from where it froze
```

Two things this buys you:

- **Simulation playback.** A flow full of `Passed(5s)` waits and `UF_STAY_UNTIL(3s, ...)` timeouts runs at whatever rate you set - drive a day-long line cycle in seconds for testing, or slow it down to watch.
- **A pause that is actually correct.** During an e-stop / hold you do *not* want a 3-second "hw ready" timeout to fire just because the line sat still for 10 seconds. `Freeze()` stops every logical deadline; `Resume()` picks up exactly where it left off, so timeouts measure *running* time, not wall time.

**What scales and what does not.** The virtual clock governs *logical* waits only - `UFTimer` (`Elapsed`/`Passed`/`HeldFor`) and `UF_STAY_UNTIL`. It deliberately does **not** touch `UF_ASYNC` / `UF_ASYNC_TIMEOUT` deadlines or the pump's own sleep: a real network call does not finish faster because you scaled the sim, so those stay on real wall-clock.

**Binding.** A timer built as `uniflow::UFTimer{rt.clock()}` follows that runtime's clock. A plain `uniflow::UFTimer{}` uses real time - handy when you specifically want a wall-clock measurement that ignores scale/freeze.

> Changing scale or freezing is *continuous* - the clock rebases on each change, so `Elapsed()` never jumps at the moment you call `SetScale` / `Freeze`.

---

## Chapter 11. Driving a flow from the outside - event threads and `Wake`

Real systems get poked by threads that are not the pump: a socket receiving a message, a device-driver callback, a GUI button. You want that event to start (or feed) a flow *immediately*, not on the next 20ms poll.

**Starting a flow from another thread is safe.** `UF_START_FLOW` (and `StartFlow`) take the module's lock internally, so you can call them from any thread:

```cpp
// on some I/O thread:
void OnNetworkMessage(Message m) {
    UF_START_FLOW(App::inst().handler, OnHandle_Begin, std::move(m));
}
```

The catch: the pump may be napping (`stay_sleep_ms`, default 20ms) when your event lands, so the first step could run up to 20ms late. So `UF_START_FLOW` / `Post` **wake the pump for you** - they nudge it out of its nap so the freshly-armed flow runs on the very next round. You can also call it explicitly:

```cpp
rt.Wake();   // pump out of its nap now, not at the next poll tick
```

When would you call `Wake()` yourself? When you mutated module state through your own channel (not via a flow) and want it serviced now. The idiomatic way to touch another runtime's state from outside is to `UF_POST` a callback (Chapter 9) - and `Post` already wakes the pump:

```cpp
UF_POST(rt, [&] { App::inst().handler.Enqueue(m); });   // runs on the pump, wakes it
```

> **Do not** poke a module's members directly from an outside thread - that races with the pump. Start a flow, or `Post` a callback; both run on the pump thread and both wake it. `Wake()` is just the low-level "stop napping" primitive the two build on.

One more place this matters: a `UF_ASYNC` job finishing also wakes the pump, so the continuation step catches the result right away instead of up to a poll-interval late.

---

## Chapter 12. Controlling a flow's lifecycle - idle, wait, stop

A module is either *idle* (no flow running) or *busy*. Three calls manage that lifecycle.

- **`IsIdle()`** - is the module free? An orchestrator checks this before starting work on a peer; `StartFlow` itself returns `false` and does nothing if a flow is already running, so a guard reads naturally:

```cpp
if (worker.IsIdle())
    UF_START_FLOW(worker, OnWork_Begin, job);
```

- **`WaitUntilIdle()`** - block the *calling* thread until the flow finishes. This is how `main()` waits for work to drain before exiting. Call it from the owning thread, never from inside a step (a step blocking on its own pump would deadlock):

```cpp
UF_START_FLOW(pipe, OnPipe_Fetch);
pipe.WaitUntilIdle();          // main thread parks here until the flow ends
```

**Stopping a running flow is cooperative.** There is no `Cancel()` that yanks a flow mid-step - that would risk tearing down state a step is in the middle of using. Instead a flow ends when a step *chooses* to: it checks a stop signal and returns `Done()` or `Fail()`. Every long-running flow uses this pattern:

```cpp
StepResult OnRun_Tick() {
    if (GlobalEnv::Stopping()) {     // your own flag, set from anywhere
        // ... release anything this flow holds ...
        return Done();
    }
    // ... normal work ...
    return Stay();
}
```

Because the check is just a step reading a flag, *you* decide where it is safe to stop - between motions, not mid-motion. For an orderly shutdown of many modules, set the flag, then `WaitUntilIdle()` each one:

```cpp
GlobalEnv::RequestStop();
for (auto* m : all_modules) m->WaitUntilIdle();
```

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

This pattern shows up in [cnc_pickers' UF_Orchestrator](cpp/examples/cnc_pickers/uf_orchestrator.cpp), [queue_drain's UF_Sender](cpp/examples/queue_drain/uf_sender.cpp), [message_dispatch's UF_Professor/UF_Friend](cpp/examples/message_dispatch/uf_professor.cpp) - almost every *real* uniflow module has this skeleton.

---

## Next

- [EXAMPLES.md](EXAMPLES.md) - six working projects walked through
- [DESIGN.md](DESIGN.md) - design rationale (currently Korean)
- [uniflow.hpp](cpp/uniflow.hpp) - the header itself, with substantial comments

Issues and PRs welcome.
