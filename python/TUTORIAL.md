# uniflow.py Tutorial

> 🌐 Language: **English** | [한국어](TUTORIAL.kr.md)

A short, Python-idiomatic tour of `uniflow.py`. Each chapter is a small, runnable program. The Python port keeps the C++ mental model but drops the machinery (no macros, no templates) - steps are just methods that return `self.Next(...)` / `self.Stay()` / `self.Done()` / `self.Fail()`.

> API reference: [PYTHON_PORT.md](PYTHON_PORT.md). For the deeper conceptual treatment (single-pump model, observer hooks, cross-runtime), the C++ tutorial [../cpp/TUTORIAL.md](../cpp/TUTORIAL.md) covers the same ideas in more detail.

Every example assumes `uniflow.py` is importable (it is a single standard-library-only file):

```python
import uniflow as uf
```

---

## Chapter 1. A one-step module

The smallest module: one class deriving from `uf.Uniflow`, one step method, one `Done()`.

```python
import uniflow as uf

class Hello(uf.Uniflow):
    def on_begin(self):
        print("hello from a step")
        return self.Done()

rt = uf.Runtime()
h = Hello(rt)
h.start(h.on_begin)        # arm the flow at on_begin
rt.wait_until_idle()       # block until every module is idle
rt.stop()
```

- `uf.Runtime()` spins up one pump thread.
- `Hello(rt)` attaches the module; the pump visits it every round.
- `h.start(h.on_begin)` arms the flow; the step runs on the next round.
- Returning `self.Done()` sends the module back to idle.

> Never call a blocking `time.sleep(...)` inside a step body - it freezes the whole pump. Use `Stay()` (Chapter 3) or `run_async` (Chapter 5).

---

## Chapter 2. A chain of steps (Next)

Real flows are N steps chained with `self.Next(self.next_method)`. The flow reads top to bottom.

```python
class Greet(uf.Uniflow):
    def on_begin(self):
        print("1. hi there")
        return self.Next(self.on_middle)
    def on_middle(self):
        print("2. nice to see you")
        return self.Next(self.on_end)
    def on_end(self):
        print("3. see you again")
        return self.Done()

rt = uf.Runtime()
g = Greet(rt)
g.start(g.on_begin)
rt.wait_until_idle(); rt.stop()
```

Each `Next` schedules the next step for the *next round*. Step boundaries are exactly where you will hook in an async result (Chapter 5).

---

## Chapter 3. Polling with `Stay()` and the timer

Return `self.Stay()` to re-run the same step next round - for polling a flag or waiting on another module. The pump rests `stay_sleep` (default 20 ms) between rounds.

Because you can never `sleep` in a step, *time* is expressed with a timer you poll. Every module has a built-in `self.uf_timer` that is **auto-armed on start and on every `Next`**, so in a waiting step you just read it:

```python
class WaitReady(uf.Uniflow):
    def on_begin(self):
        return self.Next(self.on_wait)
    def on_wait(self):
        # held_for: True once the condition has stayed true for 0.05s straight
        # (settling / debounce). passed(d): True once d has elapsed (no condition).
        if self.uf_timer.held_for(hardware_ready, 0.05):
            return self.Next(self.on_go)
        return self.Stay()
    def on_go(self):
        return self.Done()
```

- `self.uf_timer.passed(d)` - has `d` seconds elapsed since the step was entered?
- `self.uf_timer.held_for(cond, d)` - has `cond` been continuously true for `d`? (a single false reading resets it)
- `self.uf_timer.elapsed()` - the raw seconds, handy for pacing/progress.

For a timer that spans steps, make your own: `t = uf.UFTimer()` as a member.

---

## Chapter 4. Many modules on one Runtime (no locks)

Attach several modules to the same `Runtime` and they all run on the same pump thread, round-robin. Shared state between them needs **no lock** - it is one thread.

```python
class Pinger(uf.Uniflow):
    def __init__(self, rt, box): super().__init__(rt); self.box = box; self.n = 3
    def on_begin(self): return self.Next(self.loop)
    def loop(self):
        if self.n <= 0: return self.Done()
        if self.box["turn"] != 0: return self.Stay()   # wait my turn
        print("ping"); self.box["turn"] = 1; self.n -= 1
        return self.Stay()

class Ponger(uf.Uniflow):
    def __init__(self, rt, box): super().__init__(rt); self.box = box; self.n = 3
    def on_begin(self): return self.Next(self.loop)
    def loop(self):
        if self.n <= 0: return self.Done()
        if self.box["turn"] != 1: return self.Stay()
        print("    pong"); self.box["turn"] = 0; self.n -= 1
        return self.Stay()

rt = uf.Runtime()
box = {"turn": 0}                 # shared, lock-free
p, q = Pinger(rt, box), Ponger(rt, box)
p.start(p.on_begin); q.start(q.on_begin)
rt.wait_until_idle(); rt.stop()
```

`box` is touched from both modules with no lock, because both run on the one pump thread.

---

## Chapter 5. Blocking work - `run_async`

Calling a slow function directly in a step stalls the pump. Hand it to the thread pool with `self.run_async(fn, then)`: it submits, Stays until done, then advances to `then` with the result in `self.async_value`.

```python
import time

class Worker(uf.Uniflow):
    def on_begin(self):
        print("submitting slow job (pump is NOT blocked)")
        return self.run_async(self._slow, self.on_done)
    @staticmethod
    def _slow():
        time.sleep(0.5)            # runs on a pool thread, not the pump
        return 9 * 9
    def on_done(self):
        print("result:", self.async_value)
        return self.Done()
```

While the job runs the pump keeps driving other modules. When the job finishes it `wake()`s the pump, so `on_done` catches the result immediately. If `_slow` raises, the flow `Fail`s and the exception is preserved.

> GIL note: I/O-bound work (network, disk) releases the GIL and gets real concurrency. CPU-bound work does not parallelize under the GIL, but offloading it still keeps the pump responsive.

---

## Chapter 6. Step timeouts - `StayUntil`

`run_async` handles a *job* that overran. But often you commanded some hardware and are now `Stay()`-polling a "done" flag or sensor. **What if it never arrives?** A jammed axis, a stuck valve - a bare `Stay()` loop polls *forever*, and the line silently hangs with no error, which on a real machine is about the worst outcome there is.

`self.StayUntil(timeout_sec, on_timeout)` is a `Stay()` with a deadline: keep polling this step, but if `timeout_sec` passes *since the step was entered*, route to `on_timeout` instead. That step is your `catch` - a guaranteed exit to a known recovery path.

The whole pattern - command, wait-with-deadline, recover:

```python
class Move(uf.Uniflow):
    def __init__(self, rt, axis, target_mm):
        super().__init__(rt); self.axis = axis; self.target = target_mm; self.tries = 0

    def on_command(self):
        self.axis.move_to(self.target)            # fire the command (non-blocking)
        return self.Next(self.wait_in_pos)

    def wait_in_pos(self):
        if self.axis.in_position():               # happy path
            return self.Next(self.on_clamp)
        # still moving - poll, but give up if it stalls past 2s
        return self.StayUntil(2.0, self.on_stalled)

    # reached ONLY if in_position never became true within 2s of entering the
    # wait step. The flow cannot hang - it always lands somewhere defined.
    def on_stalled(self):
        self.axis.abort()
        print("axis stalled before reaching target")
        return self.Fail("stalled")

    def on_clamp(self):
        return self.Done()
```

Without `StayUntil`, `wait_in_pos` returning a bare `Stay()` would spin until someone notices the line is dead. With it you are *guaranteed* to reach `on_stalled` if the move does not complete.

**The recovery step is just a step, so it can route anywhere** - a common shape is *retry, then give up*:

```python
    def wait_in_pos(self):
        if self.axis.in_position():
            return self.Next(self.on_clamp)
        return self.StayUntil(2.0, self.on_retry)

    def on_retry(self):
        self.tries += 1
        if self.tries >= 3:
            print("axis failed after 3 tries"); return self.Fail("gave up")
        self.axis.abort()
        return self.Next(self.on_command)         # re-issue -> re-enters the wait,
                                                  # restarting the 2s window
```

Re-entering `on_command` -> `wait_in_pos` is a fresh step entry, so the 2s deadline starts over for each attempt - no manual timer bookkeeping. The deadline is measured from step entry, so the repeated Stay ticks do not push it back.

> **Virtual clock.** The timer and `StayUntil` deadlines run on `rt.clock`, which you can scale or freeze: `rt.clock.set_scale(10)` plays the whole flow back 10x; `rt.clock.freeze()` / `.resume()` holds every logical timeout (e.g. during an e-stop, a 2s timeout will not fire while paused). Async/IO deadlines stay on real time.

---

## Chapter 7. Observers

Every event (flow/step/async/exception/end) flows through an `Observer`. The default `ConsoleObserver` pretty-prints to stdout. For silence or custom logging, subclass and pass it in:

```python
class Quiet(uf.Observer):
    pass                                   # override nothing -> no output

class MyObs(uf.Observer):
    def on_step_changed(self, obj, prev, nxt, elapsed_ms, ticks):
        print(f"{obj}: {prev} -> {nxt} ({elapsed_ms:.1f}ms)")
    def on_flow_ended(self, obj, action, steps, wall_ms, reason):
        if action is uf.StepAction.FAIL:
            print(f"{obj} FAILED: {reason}")

rt = uf.Runtime(observer=MyObs())          # every module on rt uses it
```

---

## Chapter 8. Driving from outside, and lifecycle

A flow is usually kicked off by something that is *not* the pump - an event thread, a socket callback, the main thread.

```python
# from any thread: starting a flow is safe and wakes the pump immediately,
# so the first step runs now, not on the next 20ms poll.
def on_message(msg):
    handler.start(lambda: handler.on_handle(msg))

# you can also nudge the pump after touching state through your own channel:
rt.wake()
```

Lifecycle control:
- `module.is_idle()` - is it free? An orchestrator checks this before starting work on a peer.
- `rt.wait_until_idle(timeout=None)` - block the calling thread until every module is idle (how `main` waits before exit). Never call it from inside a step.
- `module.cancel()` - cooperatively end a running flow (it is marked failed with reason `"cancelled"`); `rt.cancel_all()` does it for all.
- `rt.stop()` - stop the pump and shut the pool down.

---

## Next

- [PYTHON_PORT.md](PYTHON_PORT.md) - the full API table and design decisions.
- [../cpp/TUTORIAL.md](../cpp/TUTORIAL.md) - the C++ tutorial; the same concepts with the single-pump model and cross-runtime features in more depth.
- [../README.md](../README.md) - project overview.
