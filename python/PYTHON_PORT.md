# uniflow.py - Python port notes

> Language: **English** | [한국어](PYTHON_PORT.kr.md)

> API & design notes for the Python sibling (`uniflow.py`) of the C++ `uniflow.hpp`.
> **This document is limited to the uniflow library** - it carries nothing about any specific consumer (application).

---

## 1. What it is

`uniflow.py` is a Python port of `uniflow.hpp`, the single-threaded cooperative step-driven framework. Its **public surface mirrors the C++ names** so the two ports read alike (Task-Level Syntax), while it drops the C++ machinery Python does not need (CRTP / macros / templates).

Core concepts (identical to the C++ edition):
- One concern = one **module** deriving from `Uniflow`.
- A module owns one or more **tasks** (deriving from `Task`) and runs **one at a time**. A task is a unit operation: a set of step methods plus the state they share.
- A task's logic = step methods returning a `StepResult` intent. Read top to bottom. The first step is named by `Entry()`; the next step is reached only via `self.Next(self.next_step)` -> the **shape is enforced**, so any author writes it the same way. `Next` never leaves the current task.
- One `Runtime` pump thread drives every attached module round-robin, one tick each, so several modules progress on one thread concurrently. No locks.
- Blocking work is offloaded to a thread pool with `self.SubmitAsync(...)`, which returns an `AsyncId`; a later step polls it with `self.AsyncResult(id)`. The pump never blocks.

`uniflow.py` is **a single file, standard library only** (easy to vendor).

> **Units.** Durations in the Python port are **seconds** (the C++ port uses milliseconds). This applies to `rt.clock.Now()`, `UFTimer`, `StayTimeout` / `StayUntil` deadlines, and async timeouts.

---

## 2. Public API

### Modules and tasks

| Element | Contents |
|---|---|
| `Uniflow` | the **module** base. `__init__(rt, *, name=None)` (`name` is keyword-only). Owns the async slots, the built-in `self.uf_timer`, and the running-flow position |
| ↳ task binding | `AddTask(task)` (wire `task.flow()` back to this module; starts nothing), `StartTask(task) -> StartResult` (launch this module at `task.Entry()`; callable from any thread) |
| ↳ introspection | `IsIdle` (free?), `WaitUntilIdle(timeout=None)` (block until *this* module is idle), `InstanceName()`, `CurrentStepName()` / `CurrentStepOrdinal()` / `CurrentStepDescription()` (live "where is the flow now?"; `""` / `-1` when idle), `Cancel()` (end the running flow as Fail, reason `"cancelled"`), `Describe(*parts)` |
| `Task` | the **task** base (commonly nested in the module). Subclass it, define step methods, override `Entry()` (name the first step) and optionally `OnEnter()` (re-arm per-task state on entry) |
| ↳ reach the module | `self.flow()` -> the owning `Uniflow` (wired by `AddTask`), e.g. `self.flow().some_attr` |
| ↳ step intents | `Stay()`, `StayTimeout(timeout_sec, timeout_step, *args, **kwargs)` (keep polling the current step, but route to `timeout_step` once `timeout_sec` of logical time passes since step entry = a step-level catch; the body owns the success path), `StayUntil(condition, settle_sec, success, timeout_sec, timeout_step)` (the same with a folded wait condition: poll `condition`, once held for `settle_sec` go to `success`, else on timeout go to `timeout_step`), `Next(fn, *args, **kwargs)` (advance to a sibling step, passing args), `Done()`, `Fail(reason="")`, `Describe(*parts)` (one-line "what am I doing" for logs; shown on the next transition then cleared) |
| ↳ launch | `StartFlow() -> StartResult` (sugar for `module.StartTask(self)`), `StartTask(other_task)` (switch the module to another of its tasks mid-flow; the only in-task way to cross tasks) |
| ↳ async | `SubmitAsync(fn, label, timeout_sec=None, *args) -> AsyncId` (offload blocking work; `fn` is a module-level / static function with no task access; `timeout_sec` in real seconds, `None` = none; returns `0` if the in-flight cap was hit), `AsyncResult(id) -> AsyncOutcome`, `AnyAsyncPending() -> bool`, `ClearAsync()` (abandon every in-flight worker) |

### Intents, async, and clocks

| Element | Contents |
|---|---|
| `StepAction` | `STAY` / `NEXT` / `DONE` / `FAIL` (an intent, not a state change) |
| `StepResult` | what a step returns. `action`, `next_fn`, `next_name`, `reason`, `timeout_sec` (StayTimeout/StayUntil), `cond` / `settle_sec` / `success_fn` / `success_name` (StayUntil folded form) |
| `StartResult` | outcome of `StartFlow` / `StartTask`: `Ok` (launched) / `Busy` (a task is already running on this module) |
| `AsyncState` | `NotFound` / `Pending` / `Done` / `Failed` / `TimedOut` |
| `AsyncOutcome` | by-value snapshot from `AsyncResult(id)`. `state`, `return_value` (engaged only when Done); predicates `ok()` / `pending()` / `failed()` / `is_timeout()` / `found()` |
| `VirtualClock` | logical clock. `Now()` (seconds), `SetScale(s)` / `Scale()` (speed), `Freeze()` / `Resume()` / `Frozen()` (pause). Tracks the real monotonic clock 1:1 by default. Governs `UFTimer` / `StayTimeout` / `StayUntil` only; async/IO deadlines and the pump nap stay on real time |
| `UFTimer` | polling timer. `UFTimer(clock=None)` (real wall clock by default; pass `rt.clock` to follow that virtual clock), `Restart()`, `HeldFor(cond, seconds) -> bool` (True once `cond` has been **continuously** true for `seconds`; any false reading resets and returns False; settling / debounce), `Passed(seconds) -> bool` (fixed wait elapsed, no condition), `Elapsed()` |
| `Config` | per-Runtime sleep knobs (seconds): `idle_sleep_sec`, `stay_sleep_sec`, `step_interval_sleep_sec`, `max_inflight_async` |

### Runtime and observers

| Element | Contents |
|---|---|
| `Runtime` | pump thread + `ThreadPoolExecutor` + observer + clock. `__init__(*, threads=4, observer=None, config=None)` (default observer is `ConsoleObserver`) |
| ↳ | `WaitUntilIdle(timeout=None)` (all modules), `CancelAll()`, `Post(fn)` (run a callback on the pump thread), `Wake()` (wake a napping pump immediately; for external event threads), `SetPreRound(fn)` (hook at the top of each round), `clock` -> `VirtualClock` (`rt.clock.SetScale(10)` / `.Freeze()` / `.Resume()`), `observer`, `config`, `stop(join=True, timeout=2.0)`. Also a context manager (`with Runtime() as rt:` -> stops on exit) |
| `Observer` / `ConsoleObserver` | single exit for every event. The base `Observer` is a no-op (silent); `ConsoleObserver` (the default) pretty-prints one line per event. Hooks: `OnFlowStarted`, `OnStepChanged(obj, prev_step, next_step, description, step_ordinal, elapsed_ms, ticks)` (carries the `Describe` text), `OnStepThrew`, `OnAsyncSubmitted`, `OnAsyncCompleted(obj, job, wait_ms, had_error, timed_out)`, `OnAsyncAbandoned`, `OnAsyncHighWater`, `OnFlowEnded(obj, terminal_action, final_step_ordinal, wall_ms, reason)`. A hook exception cannot kill the pump |
| `__version__` / `VERSION` | `"1.0.0"` / `(1, 0, 0)` |

Minimal use:

```python
import uniflow

class Flow_Router(uniflow.Uniflow):
    def __init__(self, rt):
        super().__init__(rt, name="Flow_Router")
        self.ctx = self.Task_Route()
        self.AddTask(self.ctx)

    class Task_Route(uniflow.Task):
        def Entry(self):                 # flow entry: name the first step
            return self.Step1_Begin()

        def Step1_Begin(self):
            self.flow().msg = fetch()
            return self.Next(self.Step2_Done)

        def Step2_Done(self):
            return self.Done()

rt = uniflow.Runtime()
r = Flow_Router(rt)
r.ctx.StartFlow()
rt.WaitUntilIdle()
```

---

## 3. What maps to what (C++ -> Python)

The Python port mirrors the C++ public API; only the language plumbing differs.

| C++ (`uniflow.hpp`) | Python (`uniflow.py`) | Note |
|---|---|---|
| `class Flow_X : uniflow::Uniflow` | `class Flow_X(uniflow.Uniflow)` | module base; CRTP `Uniflow<Derived>` -> plain inheritance |
| `struct Task_Y : uniflow::Task<Flow_X>` | `class Task_Y(uniflow.Task)` | task base; reaches the module via `flow()` |
| `AddTask(&task)` / `StartTask(task)` | `AddTask(task)` / `StartTask(task)` | bind once, launch from any thread |
| `task.StartFlow()` | `task.StartFlow()` | sugar for `module.StartTask(self)` |
| `UF_NEXT(StepFn, args...)` | `self.Next(self.StepFn, *args)` | macro -> method; args forwarded to the next step |
| `Stay()` / `Done()` / `Fail(reason)` | `Stay()` / `Done()` / `Fail(reason="")` | same intents |
| `StayUntil(ms, StepFn)` | `StayTimeout(seconds, StepFn)` | ms -> seconds; renamed (StayUntil is now the folded condition form) |
| `SubmitAsync(fn, "label", ms, args...)` | `SubmitAsync(fn, "label", seconds, *args)` | returns an `AsyncId`; `0` = rejected |
| `AsyncResult(id)` -> outcome | `AsyncResult(id)` -> `AsyncOutcome` | `ok()` / `pending()` / `failed()` / `is_timeout()`; `.return_value` |
| `UFTimer`, `HeldFor` / `Passed` / `Elapsed` | same names | bind to `rt.clock` for scale / freeze |
| `clock.SetScale` / `Freeze` / `Resume` | `rt.clock.SetScale` / `Freeze` / `Resume` | logical-time control |
| `Runtime`, `WaitUntilIdle`, `Wake`, `Post` | same names | pump + executor + observer + clock |
| `Observer` / `ConsoleObserver` | same names | base is silent; default is `ConsoleObserver` |

The class name is taken from `type(self).__name__` when `name` is omitted. `name` is **keyword-only** (`__init__(rt, *, name=None)`): a common misuse like `Flow_X(rt, x)` (a stray second positional) raises `TypeError` immediately rather than silently binding to `name`.

---

## 4. Design decisions

- **Faithful to the methodology, dropping the C++ machinery.** CRTP (`Uniflow<Derived>`) -> plain inheritance. Macros (`UF_NEXT` / `UF_START_FLOW` / `UF_ASYNC` etc.) -> methods. Class name -> `type(self).__name__`.
- **Module owns tasks; a module runs one task at a time.** Logic lives in `Task` subclasses (commonly nested), so each unit operation owns its step methods and the state they share. `Next` advances within a task; `StartTask` is the only in-task way to cross to another task of the same module.
- **Blocking work = executor offload, polled by id.** `SubmitAsync` enqueues the worker on a `ThreadPoolExecutor` and returns an `AsyncId`; the pump sweeps completed ids each round and a later step reads the `AsyncOutcome` with `AsyncResult`. A finished worker `Wake()`s the pump, so the next poll catches it without waiting a full `stay_sleep_sec`. Mind the GIL: I/O-bound work (network / gRPC) releases the GIL while blocking and gets real concurrency, but CPU-bound steps do not parallelize under the GIL - the rule is to offload CPU-heavy work too.
- **The poll cadence is managed centrally by the Runtime; there is no per-step gate.** The inter-round wait is an interruptible `Condition`, and `StartTask` / async completion / external `Wake()` interrupt it immediately - so a loose poll cadence (`stay_sleep_sec`) never delays a flow launch or an async-completion catch.
- **`HeldFor` is "satisfied continuously for `seconds`?" (settling), not "satisfied within a deadline?".** The deadline-based "bail out if it never comes" is `StayTimeout(timeout, target)`'s job (routing to a cleanup step like a try/catch's catch). The two are orthogonal: `HeldFor` watches a condition settle, `StayTimeout` provides the escape hatch when it never does.
- **`SetPreRound(fn)`**: a hook called once at the top of every round. Keeps common per-round preprocessing ("refresh / poll once per round") out of each module.
- **Deferred for now (extend as needed):** cross-runtime `PostAndWait` / `Link`, `RoundProfile` / slow-round tracing, `FlowStats`. The examples do not use them yet, so the single file stays small.

---

## 5. Status

- **Core re-aligned to the C++ design.** Modules + tasks (`Uniflow` / `Task`), step intents (`Stay` / `StayTimeout` / `StayUntil` / `Next` / `Done` / `Fail`), `StartTask` / `StartFlow` / `AddTask`, id-based async (`SubmitAsync` / `AsyncResult` / `AsyncOutcome` / `AnyAsyncPending` / `ClearAsync`), `VirtualClock` (`SetScale` / `Freeze` / `Resume`), `UFTimer`, `Config`, `Runtime` (pump + executor + `Post` / `Wake` / `SetPreRound` / `WaitUntilIdle` / `CancelAll` / `stop`), `Observer` / `ConsoleObserver`.
- **Six examples ported** and shipped under [examples/](examples/), mirroring the C++ ([../cpp/examples](../cpp/examples/)) and C# ([../cs/examples](../cs/examples/)) sets: `simulator.py` (virtual clock - scale / freeze), `shared_ostream.py` (lock-free shared sink), `message_dispatch.py` (routing + async poll), `pick_and_place.py` (orchestrator + multi-task module + async-poll acks), `queue_drain.py`, `city_traffic.py`.
- **Verified on Python 3.14**: the tutorial's key samples (a Task `Next` chain + `StartFlow` + `WaitUntilIdle`; a `SubmitAsync` + `AsyncResult` poll returning the worker result; `VirtualClock.SetScale` shrinking a `StayUntil` deadline; `Freeze` / `Resume` holding logical time; `UFTimer.HeldFor` settling) all run clean.

---

## 6. Layout / git

- Canonical location: `python/uniflow.py` (the C++ sibling is `cpp/uniflow.hpp`). Ships together in the uniflow mono-repo.
- Organized into per-language directories (`cpp/`, `python/`, `cs/`). When one language outgrows a single file, split inside its directory.
