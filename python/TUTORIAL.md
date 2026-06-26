# uniflow.py Tutorial

> Language: **English** | [한국어](TUTORIAL.kr.md)

A Python-idiomatic tour of `uniflow.py`. Each chapter is a small, runnable program. The Python port keeps the C++ mental model and mirrors its public names (Task-Level Syntax) but drops the machinery (no macros, no templates): a **module** subclasses `uniflow.Uniflow` and owns one or more **tasks**; a **task** subclasses `uniflow.Task` and owns its step methods. A step is a method that returns an intent - `self.Next(...)` / `self.Stay()` / `self.Done()` / `self.Fail()`.

> API reference: [PYTHON_PORT.md](PYTHON_PORT.md). For the deeper conceptual treatment (single-pump model, observer hooks, cross-runtime), the C++ tutorial [../cpp/TUTORIAL.md](../cpp/TUTORIAL.md) covers the same ideas in more detail.

Every example assumes `uniflow.py` is importable (it is a single standard-library-only file):

```python
import uniflow
```

> **Examples.** The six worked examples referenced here live in [python/examples](examples/) - `simulator.py`, `shared_ostream.py`, `message_dispatch.py`, `pick_and_place.py`, `queue_drain.py`, `city_traffic.py`. They mirror the C++ set in [../cpp/examples](../cpp/examples/) and the C# set in [../cs/examples](../cs/examples/), so the same program reads alike in all three languages.

> **Units.** Durations in the Python port are **seconds** (the C++ port uses milliseconds). `rt.clock.Now()`, `UFTimer`, and `StayTimeout` / `StayUntil` deadlines are all in seconds.

---

## Chapter 1. A module, a task, one step

The smallest unit is a module deriving from `uniflow.Uniflow` that owns one task. The task derives from `uniflow.Task`, names its first step in `Entry()`, and the step returns `Done()`.

```python
import uniflow

class Flow_Hello(uniflow.Uniflow):
    def __init__(self, rt):
        super().__init__(rt, name="Flow_Hello")
        self.ctx = self.Task_Hello()      # the module's one task
        self.AddTask(self.ctx)            # wire flow() back-pointer; starts nothing

    class Task_Hello(uniflow.Task):
        def Entry(self):                  # name the first step
            return self.Step1_Greet()

        def Step1_Greet(self):
            print("hello from a step")
            return self.Done()

rt = uniflow.Runtime()
h = Flow_Hello(rt)
h.ctx.StartFlow()             # launch the task; the first step runs next round
rt.WaitUntilIdle()            # block until every module is idle
rt.stop()
```

- `uniflow.Runtime()` spins up one pump thread.
- `Flow_Hello(rt)` attaches the module; the pump visits it every round.
- `AddTask(self.ctx)` binds the task so its steps can reach the module via `self.flow()`.
- `h.ctx.StartFlow()` launches the task; its `Entry()` runs on the next round. It returns `StartResult.Ok`, or `StartResult.Busy` if a task is already running on this module.
- Returning `self.Done()` sends the module back to idle.

> Never call a blocking `time.sleep(...)` inside a step body - it freezes the whole pump. Use `Stay()` (Chapter 3) or `SubmitAsync` (Chapter 5).

---

## Chapter 2. A chain of steps (Next)

Real tasks are N steps chained with `self.Next(self.next_step)`. Steps are sibling methods on the same task; the task reads top to bottom.

```python
import uniflow

class Flow_Greet(uniflow.Uniflow):
    def __init__(self, rt):
        super().__init__(rt, name="Flow_Greet")
        self.ctx = self.Task_Greet()
        self.AddTask(self.ctx)

    class Task_Greet(uniflow.Task):
        def Entry(self):
            return self.Step1_Hi()

        def Step1_Hi(self):
            print("1. hi there")
            return self.Next(self.Step2_Nice)

        def Step2_Nice(self):
            print("2. nice to see you")
            return self.Next(self.Step3_Bye)

        def Step3_Bye(self):
            print("3. see you again")
            return self.Done()

rt = uniflow.Runtime()
g = Flow_Greet(rt)
g.ctx.StartFlow()
rt.WaitUntilIdle(); rt.stop()
```

Each `Next` schedules the next step for the *next round*. `Next` never leaves the current task - it only advances to a sibling step. Step boundaries are where an async result is hooked in (Chapter 5).

Arguments can be passed forward: `self.Next(self.Step2_Wait, job_id)` calls `Step2_Wait(self, job_id)` next round - the canonical way to carry an `AsyncId` from a submit step to its poll step.

---

## Chapter 3. Polling with `Stay()` and the timer

Return `self.Stay()` to re-run the same step next round - for polling a flag or waiting on another module. The pump rests `stay_sleep_sec` (default 20 ms) between all-Stay rounds.

Because you can never `sleep` in a step, *time* is expressed with a timer you poll. Every module has a built-in `self.uf_timer` (bound to the runtime clock) that is **re-armed on every step change** - a `Next`, a `StayTimeout` / `StayUntil` timeout, a task switch, or flow start - but **not** on a `Stay`, so within one step it keeps counting while you poll. Read it from the module via `self.flow()`. To auto-reset your own member timers the same way, create them with `self.NewAutoTimer()`; for a timer you reset yourself, own a plain `UFTimer` and arm it in `OnEnter()`:

```python
import uniflow

def hardware_ready():
    ...   # read a sensor / flag

class Flow_WaitReady(uniflow.Uniflow):
    def __init__(self, rt):
        super().__init__(rt, name="Flow_WaitReady")
        self.ctx = self.Task_Wait()
        self.AddTask(self.ctx)

    class Task_Wait(uniflow.Task):
        def OnEnter(self):
            # re-arm per-task state on entry; bound to rt.clock (scale / freeze)
            self.settle = uniflow.UFTimer(self.flow()._rt.clock)

        def Entry(self):
            return self.Step1_Wait()

        def Step1_Wait(self):
            # HeldFor: True once the condition has stayed true for 0.05s straight
            # (settling / debounce). A single false reading resets the accumulator.
            if self.settle.HeldFor(hardware_ready, 0.05):
                return self.Next(self.Step2_Go)
            return self.Stay()

        def Step2_Go(self):
            return self.Done()
```

- `timer.Passed(d)` - have `d` seconds elapsed since the timer was armed?
- `timer.HeldFor(cond, d)` - has `cond` been continuously true for `d`? (a single false reading resets it)
- `timer.Elapsed()` - the raw seconds, handy for pacing / progress.

The wait-with-settle pattern above (poll a condition, hold it for `d`, else give up) is common enough that `StayUntil` folds it into one call: a wait condition, a settle window, and a timeout catch. The condition is polled each round; once it has stayed true for `settle_sec` the step goes to `success`, but if `timeout_sec` elapses first it goes to the timeout target instead. The argument order is `condition, settle_sec, success, timeout_sec, timeout_step`:

```python
def Step1_Wait(self):
    # wait for hardware_ready, hold it 0.05s to settle; give up after 5s
    return self.StayUntil(hardware_ready, 0.05, self.Step2_Go,
                          5.0, self.Step_Timeout)
```

(For a plain timeout escape where the body decides the success path itself, use `StayTimeout(timeout_sec, timeout_step)` - see Chapter 6.)

`OnEnter()` runs once each time the task is entered, before its first step - the place to re-arm per-task state. Override `Entry()` to name the first step.

---

## Chapter 4. Many modules on one Runtime (no locks)

Several modules attached to the same `Runtime` all run on the same pump thread, round-robin. Shared state between them needs **no lock**, because it is one thread. This is the `shared_ostream.py` example in miniature.

```python
import uniflow

class SharedState:                # one sink + a turn flag, touched only on the pump
    log = []
    turn = 0

class Flow_Writer(uniflow.Uniflow):
    def __init__(self, rt, text, count, turn_id):
        super().__init__(rt, name="Flow_Writer")
        self.text = text
        self.remaining = count
        self.turn_id = turn_id        # 0 or 1; write only when it is our turn
        self.ctx = self.Task_Write()
        self.AddTask(self.ctx)

    class Task_Write(uniflow.Task):
        def Entry(self):
            return self.Step1_Loop()

        def Step1_Loop(self):
            f = self.flow()
            if f.remaining <= 0:
                return self.Done()
            if SharedState.turn != f.turn_id:
                return self.Stay()            # not our turn - poll again next round
            SharedState.log.append(f.text)    # shared sink, no lock
            SharedState.turn = 1 - SharedState.turn
            f.remaining -= 1
            return self.Stay()

rt = uniflow.Runtime(observer=uniflow.Observer())     # silent observer
hello = Flow_Writer(rt, "Hello ", 3, 0)
world = Flow_Writer(rt, "World. ", 3, 1)
hello.ctx.StartFlow(); world.ctx.StartFlow()
hello.WaitUntilIdle(); world.WaitUntilIdle(); rt.stop()
print("".join(SharedState.log))    # Hello World. Hello World. Hello World.
```

`SharedState` is touched from both modules with no lock, because both run on the one pump thread. `self.flow()` reaches the owning module from inside a task step.

> Passing `observer=uniflow.Observer()` installs a **silent** observer (the base class is a no-op). The default is `ConsoleObserver`, which prints every event - useful while learning, but suppress it when your program owns stdout. See Chapter 7.

---

## Chapter 5. Blocking work - `SubmitAsync` + `AsyncResult`

Calling a slow function directly in a step stalls the pump. Hand it to the thread pool with `self.SubmitAsync(...)`, which returns an **AsyncId** immediately; carry that id to a later step and **poll** it with `self.AsyncResult(id)`. The pump never blocks.

```python
import time
import uniflow

def slow_square(n):
    time.sleep(0.5)            # runs on a pool thread, not the pump
    return n * n

class Flow_Worker(uniflow.Uniflow):
    def __init__(self, rt):
        super().__init__(rt, name="Flow_Worker")
        self.ctx = self.Task_Work()
        self.AddTask(self.ctx)

    class Task_Work(uniflow.Task):
        def Entry(self):
            return self.Step1_Submit()

        def Step1_Submit(self):
            print("submitting slow job (pump is NOT blocked)")
            # (fn, label, timeout_sec, *args); timeout_sec=None means no timeout.
            # fn is a module-level / static function - it has no access to the task.
            aid = self.SubmitAsync(slow_square, "slow_square", None, 9)
            if aid == 0:
                return self.Fail(reason="rejected: in-flight cap reached")
            return self.Next(self.Step2_Wait, aid)     # carry the id forward

        def Step2_Wait(self, aid):
            r = self.AsyncResult(aid)      # an AsyncOutcome snapshot
            if r.pending():
                return self.Stay()         # still in flight - poll again
            if not r.ok():
                return self.Fail(reason="job failed or timed out")
            print("result:", r.return_value)
            return self.Done()
```

While the job runs the pump keeps driving other modules. When the job finishes it wakes the pump, so the next `Step2_Wait` poll catches the result immediately rather than after a full `stay_sleep_sec`.

`AsyncResult(id)` returns an `AsyncOutcome` with `.state` and these predicates: `.ok()` (Done - `.return_value` is engaged), `.pending()` (still in flight), `.failed()` (the worker threw), `.is_timeout()` (missed its deadline), `.found()` (the id matched a live slot). A bad / cleared id (including `0`) reads back as `NotFound`. The module also offers `self.AnyAsyncPending()` and `self.ClearAsync()` (abandon every in-flight worker).

To give the job a deadline, pass `timeout_sec` (real seconds, the 3rd argument): `self.SubmitAsync(fn, "label", 2.0, *args)`. After the deadline the outcome reads `is_timeout()` and the worker is abandoned (it keeps running into a discarded result).

> GIL note: I/O-bound work (network, disk) releases the GIL and gets real concurrency. CPU-bound work does not parallelize under the GIL, but offloading it still keeps the pump responsive.

---

## Chapter 6. Step timeouts - `StayTimeout`

`SubmitAsync` handles a *job* that overran. A different case is commanding some hardware and then `Stay()`-polling a "done" flag or sensor, where the signal may never arrive. With a jammed axis or a stuck valve, a bare `Stay()` loop polls indefinitely, and the line hangs with no error, which on a real machine is a serious failure mode.

`self.StayTimeout(timeout_sec, on_timeout)` is a `Stay()` with a deadline: keep polling this step, but if `timeout_sec` of **logical time** passes *since the step was entered*, route to `on_timeout` instead. That step acts as a `catch` - a guaranteed exit to a known recovery path. The body still owns the happy path (it returns `Next`/`Done` itself); the deadline only guarantees an exit if the wait never resolves. (To also fold the wait condition and a settle window into the call, use `StayUntil` from Chapter 3.)

The full pattern - command, wait-with-deadline, recover:

```python
import uniflow

class Flow_Move(uniflow.Uniflow):
    def __init__(self, rt, axis, target_mm):
        super().__init__(rt, name="Flow_Move")
        self.axis = axis
        self.target = target_mm
        self.tries = 0
        self.ctx = self.Task_Move()
        self.AddTask(self.ctx)

    class Task_Move(uniflow.Task):
        def Entry(self):
            return self.Step1_Command()

        def Step1_Command(self):
            f = self.flow()
            f.axis.move_to(f.target)              # fire the command (non-blocking)
            return self.Next(self.Step2_WaitInPos)

        def Step2_WaitInPos(self):
            if self.flow().axis.in_position():    # happy path
                return self.Next(self.Step3_Clamp)
            # still moving - poll, but give up if it stalls past 2s
            return self.StayTimeout(2.0, self.Step_Stalled)

        # reached ONLY if in_position never became true within 2s of entering the
        # wait step. The flow cannot hang - it always lands somewhere defined.
        def Step_Stalled(self):
            self.flow().axis.abort()
            print("axis stalled before reaching target")
            return self.Fail(reason="stalled")

        def Step3_Clamp(self):
            return self.Done()
```

Without `StayTimeout`, `Step2_WaitInPos` returning a bare `Stay()` would spin until someone notices the line is dead. With it, reaching `Step_Stalled` is guaranteed if the move does not complete.

The recovery step is itself a step, so it can route anywhere. A common shape is *retry, then give up*:

```python
        def Step2_WaitInPos(self):
            if self.flow().axis.in_position():
                return self.Next(self.Step3_Clamp)
            return self.StayTimeout(2.0, self.Step_Retry)

        def Step_Retry(self):
            f = self.flow()
            f.tries += 1
            if f.tries >= 3:
                print("axis failed after 3 tries")
                return self.Fail(reason="gave up")
            f.axis.abort()
            return self.Next(self.Step1_Command)   # re-issue -> re-enters the wait,
                                                   # restarting the 2s window
```

Re-entering `Step1_Command` -> `Step2_WaitInPos` is a fresh step entry, so the 2s deadline starts over for each attempt, with no manual timer bookkeeping. The deadline is measured from step entry, so the repeated Stay ticks do not push it back.

> **Virtual clock.** The timer and `StayTimeout` / `StayUntil` deadlines run on `rt.clock`, which you can scale or freeze: `rt.clock.SetScale(10)` plays the whole flow back 10x; `rt.clock.Freeze()` / `.Resume()` holds every logical timeout (e.g. during an e-stop, a 2s timeout will not fire while paused). Async / IO deadlines stay on real time. The `simulator.py` example drives all of this live from the keyboard.

---

## Chapter 7. Observers

Every event (flow / step / async / exception / end) flows through an `Observer`. The default `ConsoleObserver` pretty-prints to stdout. For silence or custom logging, subclass and pass it in:

```python
import uniflow

class Flow_Observed(uniflow.Observer):   # an empty Observer -> no output (silent)
    pass

class MyObserver(uniflow.Observer):
    def OnStepChanged(self, obj, prev_step, next_step, description,
                      step_ordinal, elapsed_ms, ticks):
        print(f"{obj}: {prev_step} -> {next_step}  {description} ({elapsed_ms:.1f}ms)")

    def OnFlowEnded(self, obj, terminal_action, final_step_ordinal, wall_ms, reason):
        if terminal_action is uniflow.StepAction.FAIL:
            print(f"{obj} FAILED: {reason}")

rt = uniflow.Runtime(observer=MyObserver())      # every module on rt uses it
```

The `description` is whatever the step wrote with `self.Describe(...)` - a one-line "what am I doing right now" for logs, printed once when the step transitions and then cleared:

```python
        def Step2_WaitInPos(self):
            self.Describe("approaching ", self.flow().target, " mm")   # shows in OnStepChanged
            if self.flow().axis.in_position():
                return self.Next(self.Step3_Clamp)
            return self.Stay()
```

You can also read it live with `module.CurrentStepDescription()` (and `CurrentStepName()` / `CurrentStepOrdinal()` for where the flow is right now). A renderer flow uses exactly these reads - see `simulator.py` and `message_dispatch.py`.

> The full observer surface is in [PYTHON_PORT.md](PYTHON_PORT.md): `OnFlowStarted`, `OnStepChanged`, `OnStepThrew`, `OnAsyncSubmitted`, `OnAsyncCompleted`, `OnAsyncAbandoned`, `OnAsyncHighWater`, `OnFlowEnded`. Override only the events you care about; a hook exception cannot kill the pump.

---

## Chapter 8. Driving from outside, and lifecycle

A task is usually launched by something that is *not* the pump - an event thread, a socket callback, the main thread. `StartFlow()` (and `StartTask`) is safe from any thread and wakes the pump immediately, so the first step runs now, not on the next 20 ms poll.

```python
# from any thread: launching a task is safe and wakes the pump immediately.
def on_message(msg):
    handler.current = msg
    handler.ctx.StartFlow()        # Ok if launched, Busy if one is already running

# you can also nudge the pump after touching state through your own channel:
rt.Wake()
```

An **orchestrator** drives this pattern at line scale: one perpetual task whose single step checks every module's `IsIdle()` each round and launches that module's next task from plain member reads - the worker modules never sequence themselves. That is the shape of `pick_and_place.py`.

Lifecycle control:
- `module.IsIdle` - is it free? An orchestrator checks this before launching a peer's next task. (`CurrentStepName()` / `CurrentStepOrdinal()` / `CurrentStepDescription()` report where a running flow is; `""` / `-1` when idle.)
- `module.WaitUntilIdle(timeout=None)` - block the calling thread until *this* module is idle.
- `rt.WaitUntilIdle(timeout=None)` - block until *every* module is idle (how `main` waits before exit). Never call either from inside a step.
- `module.Cancel()` - cooperatively end a running flow (it is marked failed with reason `"cancelled"`); `rt.CancelAll()` does it for all.
- `rt.stop()` - stop the pump and shut the pool down. A `Runtime` is also a context manager (`with uniflow.Runtime() as rt:`), which stops on exit.

A common cooperative shutdown (used by every console example) is a module-level "stop" flag that each step checks and returns `Done()` on, after which `WaitUntilIdle()` returns and `rt.stop()` tears everything down.

---

## Next

- [PYTHON_PORT.md](PYTHON_PORT.md) - the full API table and design decisions.
- [python/examples](examples/) - the six worked examples (mirroring [../cpp/examples](../cpp/examples/) and [../cs/examples](../cs/examples/)).
- [../cpp/TUTORIAL.md](../cpp/TUTORIAL.md) - the C++ tutorial; the same concepts with the single-pump model and cross-runtime features in more depth.
- [../README.md](../README.md) - project overview.
