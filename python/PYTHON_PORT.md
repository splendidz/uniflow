# uniflow.py - Python port notes

> 🌐 Language: **English** | [한국어](PYTHON_PORT.kr.md)

> Design / progress record for the Python sibling (`uniflow.py`) of the C++ `uniflow.hpp`.
> **This document is limited to the uniflow library** - it carries nothing about any specific consumer (application).

---

## 1. What it is

`uniflow.py` is a Python implementation of `uniflow.hpp`, the single-threaded cooperative step-driven framework. It is **faithful to the mental model and behavior**, but does not port the C++ machinery that Python does not need (CRTP / macros / templates).

Core concepts (identical to the C++ edition):
- One concern = one class deriving from `Uniflow`.
- That class's logic = a chain of member functions returning `StepResult`. Read top to bottom.
- The next step is reached only via `self.Next(self.next_member)` -> the **shape is enforced**, so any author writes it the same way.
- One `Runtime` pump thread drives every attached module round-robin, one tick each -> several modules progress on one thread **as if in parallel**. No locks.
- Blocking work is offloaded to a thread pool with `self.run_async(fn, then=...)`, returning control to the pump via Stay until it completes.

`uniflow.py` is **a single file, standard library only** (easy to vendor).

---

## 2. Public API

| Element | Contents |
|---|---|
| `StepAction` | `STAY` / `NEXT` / `DONE` / `FAIL` (an intent, not a state change) |
| `StepResult` | step return value. `action`, `next_fn`, `next_name`, `reason`, `timeout_sec` (StayUntil only) |
| `VirtualClock` | logical clock. `now()`, `set_scale(s)` (speed), `freeze()`/`resume()` (pause), `scale`/`frozen`. Tracks the real clock 1:1 by default. Applies to UFTimer/StayUntil only; async/IO and the pump nap stay on real time |
| `UFTimer` | polling timer. `UFTimer(clock=None)` (real clock by default; pass `rt.clock` to follow that virtual clock), `init()` (=`restart`), `held_for(cond, duration)->bool` (True once `cond` has been **continuously** true for `duration`; any false reading resets and returns False; settling/debounce), `passed(duration)->bool` (fixed wait elapsed, no condition), `elapsed()` |
| `Uniflow` | inheritance base. `__init__(rt, *, name=None)` |
| ↳ step helpers | `Stay()`, `StayUntil(timeout_sec, on_timeout, *args, **kwargs)` (keep polling the current step, but route to `on_timeout` once `timeout_sec` passes since step entry = a step-level catch), `Next(fn, *args, **kwargs)` (like C++ `UF_NEXT(fn, args...)` - passes args to the next step), `Done()`, `Fail(reason="")`, `Describe(*parts)` (one-line "what am I doing" for logs; shown on the next transition then cleared) |
| ↳ timer | `self.uf_timer` (built-in `UFTimer`) - **auto-init on start and every Next transition**. In a waiting step, `held_for(cond, duration)` checks whether the condition stayed stable for `duration` within that step (no manual `init()`). For measurement across step boundaries, use a separate `uf.UFTimer()` |
| ↳ async | `run_async(fn, then, timeout=None)` - offload blocking work; the result is in `self.async_value`. When the job finishes it auto-`wake()`s the pump. If `timeout` (real seconds) is exceeded, it abandons the worker, sets `self.async_timed_out=True`, and proceeds to `then` (check the flag there - like C++ `is_timeout()`) |
| ↳ control | `start(first_step, *args, **kwargs)->bool` (args forwarded to the entry step; returns False if a flow is already running - does not hijack), `wait_until_idle(timeout=None)` (block until *this* module is idle), `cancel()`, `is_idle()`, `current_step_name` / `current_step_ordinal` / `current_step_description` (live "where is the flow now?"; `""`/`-1` when idle), `.name` |
| `Runtime` | pump thread + `ThreadPoolExecutor` + Observer |
| ↳ | `wait_until_idle(timeout)` (all modules), `cancel_all()`, `any_failed()`, `submit(fn)`, `set_pre_round(fn)`, `wake()` (wake a napping pump immediately; for external event threads), `clock` (this runtime's logical clock = `VirtualClock`; `rt.clock.set_scale(10)`/`.freeze()`), `stop()` |
| `Observer` / `ConsoleObserver` | single exit for every event (flow/step/async/threw/ended). `on_step_changed(obj, prev, next, description, elapsed_ms, ticks)` carries the `Describe` text; `on_async_completed(obj, wait_ms, had_error, timed_out)` |

Minimal use:

```python
import uniflow as uf

class OrderRouter(uf.Uniflow):
    def on_begin(self):              # flow entry
        self.msg = fetch()
        return self.Next(self.on_done)
    def on_done(self):
        return self.Done()

rt = uf.Runtime()
r = OrderRouter(rt)
r.start(r.on_begin)
rt.wait_until_idle()
```

---

## 3. Design decisions

- **Faithful to the methodology, dropping the C++ machinery.** CRTP (`Uniflow<Derived>`) -> plain inheritance. Macros (`UF_NEXT`/`UF_START_FLOW`/`UF_ASYNC` etc.) -> methods / plain functions. Class name -> `type(self).__name__`.
- **`name` is keyword-only** (`__init__(rt, *, name=None)`): a common misuse like `Foo(rt, x)` (a second positional) does not silently bind to `name` - it raises `TypeError` immediately.
- **Blocking work = executor offload.** `run_async` polls the future and Stays until done. Mind the GIL: I/O-bound work (e.g. network/gRPC) releases the GIL while blocking and gets real concurrency, but CPU-bound steps do not parallelize under the GIL - the rule is to offload CPU-heavy work too.
- **`set_pre_round(fn)`**: a hook called once at the top of every round. Keeps common per-round preprocessing ("refresh/poll once per round") out of each module.
- **The poll cadence is managed centrally by the Runtime; there is no per-step gate.** The old `Stay(gate_sec)` was removed (having every step decide its own wake time is a burden). Instead the inter-round wait is an interruptible Condition, and `start()` / `run_async` completion / external events `wake()` it immediately - so a loose poll cadence (stay_sleep) never delays flow start or async-completion catch.
- **`held_for` is not "satisfied within a deadline?" but "satisfied continuously for `duration`?"** (settling). The deadline-based "bail out if it never comes" is `StayUntil(timeout, target)`'s job (routing to a cleanup step like a try/catch's catch). The two are orthogonal: `held_for` watches a condition settle, `StayUntil` provides the escape hatch when it never does.
- **Deferred for now (extend as needed):** cross-runtime `Post`/`PostAndWait`, `Link`, `RoundProfile`/slow-round tracing, `FlowStats`. Tests/examples do not use them yet, so the single file stays small.

---

## 4. Status

- ✅ **Implemented** - `StepAction`/`StepResult`, `Uniflow` (Stay/Next/Done/Fail, run_async, start/cancel/is_idle), `Runtime` (pump + executor + set_pre_round + wait_until_idle/cancel_all/any_failed/stop), `Observer`/`ConsoleObserver`.
- ✅ **Smoke-verified** (Python 3.14): (1) a member-function chain (Stay->Next->Done) runs to completion, (2) two modules interleave `A,B,A,B` on one pump (=as if parallel), (3) `run_async` offload (pump not blocked), (4) a step exception -> Fail isolation + `fail_exc` preserved + `any_failed` aggregation.

---

## 5. Layout / git

- Canonical location: `python/uniflow.py` (the C++ sibling is `cpp/uniflow.hpp`). Ships together in the uniflow mono-repo.
- Reorganized into per-language directories (`cpp/`, `python/`, later `csharp/`). When one language outgrows a single file, split inside its directory.
