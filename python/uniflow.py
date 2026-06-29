"""uniflow.py - single-threaded, step-driven cooperative async framework.

Python port of cpp/uniflow.hpp. The public surface mirrors the C++ names so the
two ports read alike (Task-Level Syntax): a module subclasses Uniflow and owns
one or more Task instances; each Task subclass owns its step METHODS and the
state they share. A step returns a StepResult intent (Stay / Next / Done / Fail).
One Runtime pump thread drives every attached module round-robin; blocking work
goes to a thread pool via SubmitAsync and is polled by AsyncId - the pump never
blocks.

모듈은 Uniflow 를 상속하고, 하나 이상의 Task 를 AddTask 로 바인딩한다. Task 서브클래스는
자기 step 메서드와 그들이 공유하는 상태를 소유한다. step 은 StepResult(Stay/Next/Done/
Fail) 의도를 반환한다. 하나의 Runtime 펌프 스레드가 attach 된 모든 모듈을 라운드로빈으로
돌린다. 블로킹 작업은 SubmitAsync 로 풀에 넘기고 AsyncId 로 폴링한다 - 펌프는 블록되지 않는다.
"""

from __future__ import annotations

# uniflow's own version. Exposed both as a semver string and a tuple.
__version__ = "1.0.0"
VERSION = (1, 0, 0)

import threading
import time
from concurrent.futures import Future, ThreadPoolExecutor
from dataclasses import dataclass
from enum import Enum
from typing import Any, Callable, List, Optional


# ----- StepAction: a step returns an intent, not a state change -----
# Four intents only: Stay, Next, Done, Fail.
class StepAction(Enum):
    STAY = "Stay"
    NEXT = "Next"
    DONE = "Done"
    FAIL = "Fail"


# ----- StartResult: outcome of StartTask / StartFlow (launching a task) -----
#   Ok   - the task was launched.
#   Busy - a task is already running on this module; nothing happened.
class StartResult(Enum):
    Ok = "Ok"
    Busy = "Busy"


@dataclass
class StepResult:
    """What a step returns: an intent, not a state change. Next/StayUntil carry a
    next_fn target; StayUntil also carries a timeout_sec (logical-time deadline
    measured from step entry; 0 means a plain Stay with no step timeout).

    StayUntil may also carry a wait condition: cond is polled each round and,
    once it has stayed true continuously for settle_sec (post-wait / settling),
    the step transitions to success_fn. next_fn stays the timeout target. cond
    None means the classic timeout-only Stay."""

    action: StepAction
    next_fn: Optional[Callable[[], "StepResult"]] = None
    next_name: str = ""
    reason: str = ""
    timeout_sec: float = 0.0
    cond: Optional[Callable[[], bool]] = None
    settle_sec: float = 0.0
    success_fn: Optional[Callable[[], "StepResult"]] = None
    success_name: str = ""


# ----- Async support -----

class AsyncState(Enum):
    NotFound = "NotFound"   # id never matched a live slot (bad id, 0, or cleared)
    Pending = "Pending"     # the worker is still in flight
    Done = "Done"           # the worker returned - return_value holds it
    Failed = "Failed"       # the worker threw
    TimedOut = "TimedOut"   # the worker missed its deadline


@dataclass
class AsyncOutcome:
    """By-value snapshot of one async submission, returned by AsyncResult(id).
    'state' classifies the slot; 'return_value' holds the worker's result and is
    engaged ONLY when state == Done. A bad / cleared / rejected id (including 0)
    reads back as NotFound."""

    state: AsyncState = AsyncState.NotFound
    return_value: Any = None

    def ok(self) -> bool:
        return self.state is AsyncState.Done

    def pending(self) -> bool:
        return self.state is AsyncState.Pending

    def failed(self) -> bool:
        return self.state is AsyncState.Failed

    def is_timeout(self) -> bool:
        return self.state is AsyncState.TimedOut

    def found(self) -> bool:
        return self.state is not AsyncState.NotFound


@dataclass
class _AsyncSlot:
    """One in-flight (or just-resolved) async submission, owned by the flow. The
    worker writes into 'fut'; the pump's per-round sweep moves it into value/exc
    and flips 'done'. The worker never touches the slot or the flow, so a dropped
    slot (ClearAsync) simply abandons the worker - it finishes harmlessly."""

    id: int
    label: str
    timeout_sec: Optional[float]
    submitted_at_sec: float
    fut: Future
    done: bool = False
    value: Any = None
    exc: Optional[BaseException] = None
    timed_out: bool = False


# ----- Observer -----
# Every framework event funnels through one of these hooks; the framework never
# touches stdout itself. Subclass and override only the events you care about.
# Called on the pump thread (async / cross-thread hooks note their thread).
class Observer:
    def OnFlowStarted(self, obj: str, first_step: str) -> None: ...
    def OnStepChanged(self, obj: str, prev_step: str, next_step: str,
                      description: str, step_ordinal: int,
                      elapsed_ms: float, ticks: int) -> None: ...
    def OnStepThrew(self, obj: str, step: str, what: str,
                    step_ordinal: int, tick: int) -> None: ...
    def OnAsyncSubmitted(self, obj: str, step: str, job: str) -> None: ...
    def OnAsyncCompleted(self, obj: str, job: str, wait_ms: float,
                         had_error: bool, timed_out: bool) -> None: ...
    def OnAsyncAbandoned(self, obj: str, job: str, pending_ms: float) -> None: ...
    def OnAsyncHighWater(self, obj: str, job: str, inflight: int) -> None: ...
    def OnFlowEnded(self, obj: str, terminal_action: StepAction,
                    final_step_ordinal: int, wall_ms: float,
                    reason: str) -> None: ...


class ConsoleObserver(Observer):
    """Pretty-prints events to stdout, one line each. Thread-safe."""

    _COL_OBJ = 16
    _COL_STEP = 28
    _COL_DESC = 30

    def __init__(self) -> None:
        self._mu = threading.Lock()

    def _print(self, line: str) -> None:
        with self._mu:
            print(line, flush=True)

    def OnFlowStarted(self, obj, first_step):
        self._print(f"[{obj:<{self._COL_OBJ}}] FLOW START  -> {first_step}")

    def OnStepChanged(self, obj, prev_step, next_step, description,
                      step_ordinal, elapsed_ms, ticks):
        transition = f"{prev_step} -> {next_step}" if next_step else prev_step
        self._print(f"[{obj:<{self._COL_OBJ}}] {transition:<{self._COL_STEP}} "
                    f"{description:<{self._COL_DESC}} #{step_ordinal:02d} "
                    f"elapsed={elapsed_ms:.2f}ms tick x{ticks}")

    def OnStepThrew(self, obj, step, what, step_ordinal, tick):
        self._print(f"[{obj:<{self._COL_OBJ}}] {step:<{self._COL_STEP}} "
                    f"[THREW] {what} #{step_ordinal:02d}")

    def OnAsyncSubmitted(self, obj, step, job):
        self._print(f"[{obj:<{self._COL_OBJ}}] {step:<{self._COL_STEP}} "
                    f"ASYNC SUBMIT  {job}")

    def OnAsyncCompleted(self, obj, job, wait_ms, had_error, timed_out):
        tag = " [TIMEOUT]" if timed_out else (" [ERROR]" if had_error else "")
        self._print(f"[{obj:<{self._COL_OBJ}}] {'':<{self._COL_STEP}} "
                    f"ASYNC DONE    {job}  wait={wait_ms:.2f}ms{tag}")

    def OnAsyncAbandoned(self, obj, job, pending_ms):
        self._print(f"[{obj:<{self._COL_OBJ}}] {'':<{self._COL_STEP}} "
                    f"[ASYNC ABANDONED]  {job}  pending={pending_ms:.2f}ms")

    def OnAsyncHighWater(self, obj, job, inflight):
        self._print(f"[{obj:<{self._COL_OBJ}}] {'':<{self._COL_STEP}} "
                    f"[ASYNC HIGHWATER]  {job} rejected, inflight={inflight}")

    def OnFlowEnded(self, obj, terminal_action, final_step_ordinal, wall_ms, reason):
        extra = f"  reason={reason}" if reason else ""
        self._print(f"[{obj:<{self._COL_OBJ}}] FLOW END  "
                    f"{terminal_action.value:<5} steps=#{final_step_ordinal:02d} "
                    f"wall={wall_ms:.2f}ms{extra}")


class _SafeObserver(Observer):
    """Wraps an observer so a hook exception cannot kill the pump thread. Runtime
    wraps every observer in this."""

    def __init__(self, inner: Observer) -> None:
        self._inner = inner

    def _call(self, name, *a):
        try:
            getattr(self._inner, name)(*a)
        except Exception:  # noqa: BLE001
            pass

    def OnFlowStarted(self, *a): self._call("OnFlowStarted", *a)
    def OnStepChanged(self, *a): self._call("OnStepChanged", *a)
    def OnStepThrew(self, *a): self._call("OnStepThrew", *a)
    def OnAsyncSubmitted(self, *a): self._call("OnAsyncSubmitted", *a)
    def OnAsyncCompleted(self, *a): self._call("OnAsyncCompleted", *a)
    def OnAsyncAbandoned(self, *a): self._call("OnAsyncAbandoned", *a)
    def OnAsyncHighWater(self, *a): self._call("OnAsyncHighWater", *a)
    def OnFlowEnded(self, *a): self._call("OnFlowEnded", *a)


# ----- Virtual clock: scalable / freezable LOGICAL time -----
class VirtualClock:
    """The time source for UFTimer and the StayUntil step deadline. By default it
    tracks the real monotonic clock 1:1, but it can be sped up / slowed down
    (SetScale) or paused (Freeze/Resume) - useful for simulation playback and for
    not letting logical timeouts fire while a line is held / e-stopped. It governs
    ONLY logical waits; async/IO deadlines and the pump's own sleeps stay on real
    wall-clock. Now() is computed fresh on every call. Thread-safe.

    UFTimer 와 StayUntil 마감의 시간원. 기본은 실제 시계를 1:1 추종하되 SetScale 로
    배속/감속, Freeze/Resume 으로 정지할 수 있다. 논리 대기에만 적용된다."""

    def __init__(self) -> None:
        self._mu = threading.Lock()
        self._base_real = time.monotonic()
        self._base_virtual = self._base_real
        self._scale = 1.0
        self._frozen = False

    def _now_locked(self) -> float:
        if self._frozen:
            return self._base_virtual
        return self._base_virtual + (time.monotonic() - self._base_real) * self._scale

    def Now(self) -> float:
        with self._mu:
            return self._now_locked()

    def _rebase_locked(self) -> None:
        # Capture current virtual time as the new origin so a scale / freeze
        # change does not discontinuously move Now().
        self._base_virtual = self._now_locked()
        self._base_real = time.monotonic()

    def SetScale(self, scale: float) -> None:
        with self._mu:
            self._rebase_locked()
            self._scale = scale

    def Scale(self) -> float:
        with self._mu:
            return self._scale

    def Freeze(self) -> None:
        with self._mu:
            self._rebase_locked()
            self._frozen = True

    def Resume(self) -> None:
        with self._mu:
            if self._frozen:
                self._base_real = time.monotonic()
                self._frozen = False

    def Frozen(self) -> bool:
        with self._mu:
            return self._frozen


# Process-wide real-time clock: the default source for a standalone UFTimer
# (scale 1, never frozen). Bind a timer to a Runtime's clock to follow its
# scale / freeze.
_REAL_CLOCK = VirtualClock()


def RealClock() -> VirtualClock:
    return _REAL_CLOCK


class UFTimer:
    """Polling timer for step waits. HeldFor answers "has the condition STAYED
    true long enough?" (settling); Elapsed / Passed answer the raw "how long since
    armed?". A standalone timer reads the process real clock; UFTimer(rt.clock)
    binds it to the runtime's virtual clock (scale / freeze)."""

    def __init__(self, clock: Optional[VirtualClock] = None) -> None:
        self._clk = clock or _REAL_CLOCK
        self._armed_at = self._clk.Now()
        self._cond_since: Optional[float] = None

    def Restart(self) -> None:
        self._armed_at = self._clk.Now()
        self._cond_since = None

    def Elapsed(self) -> float:
        return self._clk.Now() - self._armed_at

    def Passed(self, seconds: float) -> bool:
        return self.Elapsed() >= seconds

    def HeldFor(self, condition, seconds: float) -> bool:
        # True once 'condition' has held continuously for 'seconds' since it first
        # turned true; any read of false resets the accumulator. Level semantics:
        # once satisfied it keeps returning true while the condition stays true.
        cond = condition() if callable(condition) else condition
        if not cond:
            self._cond_since = None
            return False
        if self._cond_since is None:
            self._cond_since = self._clk.Now()
        return (self._clk.Now() - self._cond_since) >= seconds


# ----- Config: per-Runtime tuning -----
@dataclass
class Config:
    """Pump-thread sleep knobs (seconds). idle: no flow running anywhere; stay:
    flows running but every active module Stay'd this round; step: at least one
    module advanced this round (0 = burst)."""

    idle_sleep_sec: float = 0.001
    stay_sleep_sec: float = 0.02
    step_interval_sleep_sec: float = 0.0
    max_inflight_async: int = 64


def _fn_name(fn) -> str:
    return getattr(fn, "__name__", "?")


def _bind(fn, args, kwargs):
    # Bind args into a 0-arg callable; the framework calls steps with no args and
    # keeps the readable name separately in StepResult.next_name.
    if args or kwargs:
        return lambda: fn(*args, **kwargs)
    return fn


# ----- Task<Flow> - the per-task base a user task derives from -----
class Task:
    """A unit operation: a set of step methods that share the state this object
    owns. Subclass it, define step methods, override Entry() to name the first
    step, and optionally OnEnter() to re-arm per-task state on entry. flow()
    reaches the owning module (wired by AddTask). Launch with self.StartFlow() or
    module.StartTask(task); a module runs one task at a time.

    단위동작: 자기 상태를 소유한 step 메서드 묶음. 상속해서 step 메서드를 정의하고
    Entry() 로 첫 step 을, 선택적으로 OnEnter() 로 진입 시 상태 재무장을 한다."""

    def __init__(self) -> None:
        self._flow: Optional["Uniflow"] = None

    # ----- overridable -----
    def Entry(self) -> StepResult:
        raise NotImplementedError("Task subclass must override Entry()")

    def OnEnter(self) -> None:
        """Re-arm per-task members here; called once each time the task is
        entered, before its first step. Default: nothing."""

    # ----- framework-internal: AddTask wires this once -----
    def _bind_flow(self, flow: "Uniflow") -> None:
        self._flow = flow

    # ----- reach the owning module -----
    def flow(self) -> "Uniflow":
        return self._flow

    # Class name of this task ("Task_Pick"), the per-task identity the module
    # reports through CurrentTaskName().
    def Name(self) -> str:
        return type(self).__name__

    # ----- step intents -----
    def Stay(self) -> StepResult:
        return StepResult(StepAction.STAY)

    def Done(self) -> StepResult:
        return StepResult(StepAction.DONE)

    def Fail(self, reason: str = "") -> StepResult:
        return StepResult(StepAction.FAIL, reason=reason)

    def Next(self, fn: Callable[..., StepResult], *args, **kwargs) -> StepResult:
        # Advance to a sibling step of THIS task; bound args are passed to fn.
        return StepResult(StepAction.NEXT,
                          next_fn=_bind(fn, args, kwargs),
                          next_name=_fn_name(fn))

    def StayUntil(self, condition: Callable[[], bool], settle_sec: float,
                  success: Callable[..., StepResult],
                  timeout_sec: float,
                  timeout_step: Callable[..., StepResult]) -> StepResult:
        # Wait for 'condition' with a settle window and a timeout catch, all in
        # one step intent. Each round 'condition' is polled; once it has stayed
        # true continuously for 'settle_sec' (post-wait / settling) the step
        # transitions to 'success'. If 'timeout_sec' of LOGICAL time elapses
        # since step entry first, it transitions to 'timeout_step' instead. The
        # deadline is measured from step entry, not from this call. For a plain
        # timeout escape with no condition (the body decides success), use
        # StayTimeout.
        #
        #   return self.StayUntil(sensor_on, 3, self.Step_Clamp,
        #                         100, self.Step_Error)
        return StepResult(StepAction.STAY,
                          next_fn=timeout_step,
                          next_name=_fn_name(timeout_step),
                          timeout_sec=timeout_sec,
                          cond=condition,
                          settle_sec=settle_sec,
                          success_fn=success,
                          success_name=_fn_name(success) if success else "")

    def StayTimeout(self, timeout_sec: float,
                    timeout_step: Callable[..., StepResult], *args,
                    **kwargs) -> StepResult:
        # Plain step deadline (the original StayUntil): keep polling THIS step,
        # but if 'timeout_sec' of LOGICAL time elapses since step entry,
        # transition to 'timeout_step' - the step-level "catch". The body decides
        # the success path with its own Next/Done/Fail; this only guarantees an
        # exit if the wait never resolves. *args/**kwargs bind to timeout_step.
        #
        #   return self.StayTimeout(2.0, self.Step_AckTimeout)
        return StepResult(StepAction.STAY,
                          next_fn=_bind(timeout_step, args, kwargs),
                          next_name=_fn_name(timeout_step),
                          timeout_sec=timeout_sec)

    def StartTask(self, other_task: "Task") -> StartResult:
        # Switch the module to another of its tasks mid-flow (rare). Next never
        # leaves the current task; this is the only in-task way to cross over.
        return self._flow._switch_task(self, other_task)

    def StartFlow(self) -> StartResult:
        # Launch THIS task from any thread; sugar for module.StartTask(self).
        return self._flow.StartTask(self)

    def Describe(self, *parts) -> None:
        self._flow.Describe(*parts)

    # ----- async (forwarded to the owning module) -----
    def SubmitAsync(self, fn: Callable[..., Any], label: str,
                    timeout_sec: Optional[float] = None, *args) -> int:
        return self._flow._submit_async(fn, label, timeout_sec, args)

    def AsyncResult(self, async_id: int) -> AsyncOutcome:
        return self._flow._async_result(async_id)

    def AnyAsyncPending(self) -> bool:
        return self._flow._any_async_pending()

    def ClearAsync(self) -> None:
        self._flow._clear_async()


# ----- Uniflow: module base -----
class Uniflow:
    """A module: holds one or more Task instances and runs ONE at a time. Attaches
    to a Runtime in __init__ and is driven round-robin by that runtime's pump.
    Bind tasks with AddTask(task) once each in __init__; launch with
    StartTask(task) (or task.StartFlow())."""

    def __init__(self, rt: "Runtime", *, name: Optional[str] = None) -> None:
        # name is keyword-only on purpose - a stray positional second arg raises
        # TypeError rather than silently binding to name.
        self._rt = rt
        self._name = name or type(self).__name__
        self._lock = threading.Lock()
        self._idle_cv = threading.Condition(self._lock)

        # Per-module built-in timer, bound to the runtime clock (scale / freeze).
        self.uf_timer = UFTimer(rt._clock)

        # Timers re-armed on every step change (Next / StayUntil timeout / flow
        # start / task switch) but NOT on a Stay. The built-in uf_timer is always
        # registered; user member timers opt in via NewAutoTimer().
        self._auto_timers: List[UFTimer] = [self.uf_timer]

        # StayUntil-with-condition settle accumulator: the virtual-clock instant
        # the wait condition last turned true (None = not currently held). Reset
        # on every step change so settle is measured from within the step.
        self._settle_since_sec: Optional[float] = None

        self._tasks: List[Task] = []

        # -- current position within the running flow --
        self._flow_running = False
        self._active_task: Optional[Task] = None
        self._current: Optional[Callable[[], StepResult]] = None
        self._current_name = ""
        self._desc = ""

        self._step_ordinal = 0
        self._flow_t0_sec = 0.0
        self._step_real_t0_sec = 0.0      # real clock (profiling elapsed_ms)
        self._step_virt_t0_sec = 0.0    # virtual clock (StayUntil deadline)
        self._step_ticks = 0

        self.failed = False
        self.fail_reason = ""
        self.fail_exc: Optional[BaseException] = None

        # -- async slots: every in-flight / just-resolved submission for the flow.
        self._async_slots: List[_AsyncSlot] = []
        self._next_async_id = 1

        rt._attach(self)

    # ----- task binding -----
    def AddTask(self, task: Task) -> None:
        # Wire the task's flow() back-pointer so its steps reach this module and
        # task.StartFlow() knows which module to launch. Does NOT start anything.
        task._bind_flow(self)
        self._tasks.append(task)

    # ----- timers -----
    def NewAutoTimer(self) -> "UFTimer":
        """Create a UFTimer bound to this runtime's clock and register it for
        auto-reset: it re-arms on every step change, like the built-in uf_timer.
        For a self-managed timer, construct UFTimer(rt._clock) directly instead."""
        t = UFTimer(self._rt._clock)
        self._auto_timers.append(t)
        return t

    def _reset_step_timers(self) -> None:
        # Re-arm every auto-reset timer and clear the StayUntil settle accumulator.
        # Called on each step change (NOT on a Stay).
        for t in self._auto_timers:
            t.Restart()
        self._settle_since_sec = None

    def StartTask(self, task: Task) -> StartResult:
        # Launch this module's flow at 'task's Entry(). Callable from ANY thread.
        # Ok if launched, Busy if a task is already running on the module.
        return self._arm_flow(task)

    def _arm_flow(self, task: Task) -> StartResult:
        with self._lock:
            if self._flow_running:
                return StartResult.Busy
            now = time.monotonic()
            self._flow_running = True
            self._active_task = task
            # The entry thunk enters the unit (OnEnter) on the PUMP thread.
            self._current = self._entry_thunk(task)
            self._current_name = "Entry"
            self._desc = ""
            self._step_ordinal = 0
            self._step_ticks = 0
            self.failed = False
            self.fail_reason = ""
            self.fail_exc = None
            self._flow_t0_sec = now
            self._step_real_t0_sec = now
            self._step_virt_t0_sec = self._rt._clock.Now()
            self._async_slots = []
            self._next_async_id = 1
            self._flow_started_pending = True
        # The first step is a step change too: arm the auto-reset timers so the
        # entry step sees fresh timers, not time accrued since construction.
        self._reset_step_timers()
        self._rt.Wake()
        return StartResult.Ok

    def _entry_thunk(self, task: Task) -> Callable[[], StepResult]:
        # Runs on the pump thread: enter the unit (OnEnter / re-arm) then call its
        # Entry(). Idempotent per task so a Stay-polling entry does not re-enter.
        def thunk() -> StepResult:
            self._begin_unit(task)
            return task.Entry()
        return thunk

    def _begin_unit(self, task: Task) -> None:
        if self._active_task is not task:
            self._active_task = task
        # OnEnter is run once per (re-)entry; the entry/switch thunk drives it via
        # a flag so a Stay-polling entry step does not re-arm every round.
        if getattr(self, "_unit_entered", None) is not task:
            self._unit_entered = task
            task.OnEnter()

    def _switch_task(self, _from: Task, other: Task) -> StartResult:
        # In-task switch to another task of the same module. Runs on the pump
        # thread (called from within a step), so it installs the new task's entry
        # thunk as the current step directly; the new task enters at its Entry()
        # next round. Unlike Next, this leaves the current task. Returns Ok unless
        # the target is not bound to this module.
        if other._flow is not self:
            return StartResult.Busy
        with self._lock:
            self._current = self._entry_thunk(other)
            self._current_name = "Entry"
            self._desc = ""
            self._step_ordinal += 1
            self._step_real_t0_sec = time.monotonic()
            self._step_virt_t0_sec = self._rt._clock.Now()
            self._step_ticks = 0
            # Force re-entry of the new unit (OnEnter) next round.
            self._unit_entered = None
        # A task switch advances the step (new task's Entry next round): re-arm
        # the auto-reset timers, same as a Next transition does.
        self._reset_step_timers()
        return StartResult.Ok

    # ----- waiting / introspection -----
    def WaitUntilIdle(self, timeout: Optional[float] = None) -> bool:
        """Block the calling thread until the running flow finishes (returns at
        once if already idle). Call from the owning thread, never inside a step."""
        deadline = None if timeout is None else (time.monotonic() + timeout)
        with self._idle_cv:
            while self._flow_running:
                remaining = None
                if deadline is not None:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        return False
                self._idle_cv.wait(remaining)
            return True

    def InstanceName(self) -> str:
        return self._name

    def IsIdle(self) -> bool:
        with self._lock:
            return not self._flow_running

    def CurrentStepName(self) -> str:
        with self._lock:
            return self._current_name if self._flow_running else ""

    def CurrentStepOrdinal(self) -> int:
        with self._lock:
            return self._step_ordinal if self._flow_running else -1

    def CurrentTaskName(self) -> str:
        # Class name of the task currently running ("Task_Pick"), empty when
        # idle. Tracks an in-task StartTask switch (updated on the switch round).
        with self._lock:
            if self._flow_running and self._active_task is not None:
                return type(self._active_task).__name__
            return ""

    def CurrentStepDescription(self) -> str:
        with self._lock:
            return self._desc if self._flow_running else ""

    def Cancel(self) -> None:
        """Stop the flow immediately as Fail. Callable from pump / external."""
        with self._lock:
            if not self._flow_running:
                return
            self._flow_running = False
            self.failed = True
            self.fail_reason = "cancelled"
            self._idle_cv.notify_all()

    # ----- module-scope helpers used by Task -----
    def Describe(self, *parts) -> None:
        self._desc = "".join(str(p) for p in parts)

    def _submit_async(self, fn, label, timeout_sec, args) -> int:
        # Reject past the in-flight cap (counting only unresolved slots).
        cfg = self._rt._config
        inflight = sum(1 for s in self._async_slots if not s.done)
        if cfg.max_inflight_async != 0 and inflight >= cfg.max_inflight_async:
            self._rt._observer.OnAsyncHighWater(self._name, label, inflight)
            return 0

        fut = self._rt._executor.submit(lambda: fn(*args))
        # Wake the pump the instant the worker finishes so the next poll is not
        # delayed by a full stay_sleep_sec nap. The callback runs on a worker thread;
        # Wake() is thread-safe.
        fut.add_done_callback(lambda _f: self._rt.Wake())
        slot = _AsyncSlot(id=self._next_async_id, label=label,
                          timeout_sec=timeout_sec, submitted_at_sec=time.monotonic(),
                          fut=fut)
        self._next_async_id += 1
        self._async_slots.append(slot)
        self._rt._observer.OnAsyncSubmitted(self._name, self._current_name, label)
        return slot.id

    def _async_result(self, async_id: int) -> AsyncOutcome:
        for s in self._async_slots:
            if s.id != async_id:
                continue
            if not s.done:
                return AsyncOutcome(AsyncState.Pending)
            if s.timed_out:
                return AsyncOutcome(AsyncState.TimedOut)
            if s.exc is not None:
                return AsyncOutcome(AsyncState.Failed)
            return AsyncOutcome(AsyncState.Done, s.value)
        return AsyncOutcome(AsyncState.NotFound)

    def _any_async_pending(self) -> bool:
        return any(not s.done for s in self._async_slots)

    def _clear_async(self) -> None:
        self._drop_async_slots()

    def _drop_async_slots(self) -> None:
        # Abandon in-flight workers (they keep running into a discarded future);
        # OnAsyncAbandoned fires per abandoned worker so the leak is visible.
        for s in self._async_slots:
            if not s.done:
                pending_ms = (time.monotonic() - s.submitted_at_sec) * 1000.0
                self._rt._observer.OnAsyncAbandoned(self._name, s.label, pending_ms)
        self._async_slots = []

    # ----- pump-driven async sweep (non-blocking) -----
    def _sweep_async(self, observer: Observer) -> None:
        now = time.monotonic()
        for s in self._async_slots:
            if s.done:
                continue
            elapsed = now - s.submitted_at_sec
            ready = s.fut.done()
            timed_out = s.timeout_sec is not None and elapsed >= s.timeout_sec
            if not ready and not timed_out:
                continue
            if timed_out and not ready:
                # Deadline missed. The worker is not killed - it keeps running and
                # finishes into its (now ignored) future.
                s.value = None
                s.exc = TimeoutError(f"async timeout: {s.label}")
                s.timed_out = True
            else:
                exc = s.fut.exception()
                if exc is not None:
                    s.value = None
                    s.exc = exc
                    s.timed_out = False
                else:
                    s.value = s.fut.result()
                    s.exc = None
                    s.timed_out = False
            s.done = True
            observer.OnAsyncCompleted(self._name, s.label, elapsed * 1000.0,
                                      s.exc is not None, s.timed_out)

    # ----- one pump tick. Returns True iff a transition (Next/Done/Fail). -----
    def _execute_once(self, observer: Observer) -> bool:
        with self._lock:
            if getattr(self, "_flow_started_pending", False):
                self._flow_started_pending = False
                first = self._current_name
                fire_started = True
            else:
                fire_started = False
            running = self._flow_running
            step = self._current
            prev_name = self._current_name
        if fire_started:
            observer.OnFlowStarted(self._name, first)
        if not running or step is None:
            return False

        # 1. non-blocking async sweep
        self._sweep_async(observer)

        # 2. run the step (timed)
        self._step_ticks += 1
        try:
            result = step()
        except BaseException as e:  # noqa: BLE001
            observer.OnStepThrew(self._name, prev_name, repr(e),
                                 self._step_ordinal, self._step_ticks)
            self._end_flow(StepAction.FAIL, observer,
                           reason=f"step threw: {e!r}", exc=e)
            return True

        # A step that did an in-task StartTask returns StartResult, not a step
        # intent: the switch already installed the new task's entry thunk as
        # _current, so this round simply yields (no extra transition). The new
        # task's Entry() runs next round.
        if isinstance(result, StartResult):
            return False

        if result.action is StepAction.STAY:
            now_v = self._rt._clock.Now()
            # A StayUntil wait condition that has stayed true for settle_sec
            # (post-wait / settling) transitions to its success target. Checked
            # before the timeout so a satisfied wait wins if both are ready.
            if result.cond is not None:
                cond_true = result.cond() if callable(result.cond) else bool(result.cond)
                if cond_true:
                    if self._settle_since_sec is None:
                        self._settle_since_sec = now_v
                    if (now_v - self._settle_since_sec) >= result.settle_sec \
                            and result.success_fn is not None:
                        self._transition(observer, prev_name,
                                         result.success_fn, result.success_name)
                        return True
                else:
                    self._settle_since_sec = None
            # A StayUntil whose deadline (logical time, from step entry) has
            # passed becomes a transition to its timeout target.
            if result.timeout_sec > 0.0 and \
                    (now_v - self._step_virt_t0_sec) >= result.timeout_sec:
                self._transition(observer, prev_name, result.next_fn, result.next_name)
                return True
            return False

        if result.action is StepAction.NEXT:
            self._transition(observer, prev_name, result.next_fn, result.next_name)
            return True

        # Done / Fail
        self._end_flow(result.action, observer, reason=result.reason)
        return True

    def _transition(self, observer: Observer, prev_name: str,
                    next_fn, next_name: str) -> None:
        elapsed_ms = (time.monotonic() - self._step_real_t0_sec) * 1000.0
        observer.OnStepChanged(self._name, prev_name, next_name, self._desc,
                               self._step_ordinal, elapsed_ms, self._step_ticks)
        with self._lock:
            self._current = next_fn
            self._current_name = next_name
            self._desc = ""
            self._step_ordinal += 1
            self._step_real_t0_sec = time.monotonic()
            self._step_virt_t0_sec = self._rt._clock.Now()
            self._step_ticks = 0
        self._reset_step_timers()

    def _end_flow(self, action: StepAction, observer: Observer,
                  reason: str = "", exc: Optional[BaseException] = None) -> None:
        elapsed_ms = (time.monotonic() - self._step_real_t0_sec) * 1000.0
        prev_name = self._current_name
        observer.OnStepChanged(self._name, prev_name, "", self._desc,
                               self._step_ordinal, elapsed_ms, self._step_ticks)
        with self._lock:
            self._flow_running = False
            if action is StepAction.FAIL:
                self.failed = True
                self.fail_reason = reason
                if exc is not None:
                    self.fail_exc = exc
            wall_ms = (time.monotonic() - self._flow_t0_sec) * 1000.0
            final_ordinal = self._step_ordinal
            self._drop_async_slots()
            self._active_task = None
            self._unit_entered = None
            self._idle_cv.notify_all()
        observer.OnFlowEnded(self._name, action, final_ordinal, wall_ms, reason)


# ----- Runtime: one pump thread + executor + observer + clock -----
class Runtime:
    """Construction spins up the pump thread, the executor (thread pool), the
    observer (default ConsoleObserver), and the logical clock. The pump drives
    every attached module round-robin and sweeps completed async ids each round.
    Use as a context manager (with Runtime() as rt: ...) to shut the pump down."""

    def __init__(self, *, threads: int = 4,
                 observer: Optional[Observer] = None,
                 config: Optional[Config] = None) -> None:
        self._objects: List[Uniflow] = []
        self._objects_mu = threading.Lock()
        self._posted: List[Callable[[], None]] = []
        self._posted_mu = threading.Lock()
        self._executor = ThreadPoolExecutor(max_workers=max(1, threads),
                                             thread_name_prefix="uf-pool")
        self._observer = _SafeObserver(observer or ConsoleObserver())
        self._config = config or Config()
        self._clock = VirtualClock()

        self._stop = threading.Event()
        # Interruptible inter-round wait. Wake() raises _wake_requested and
        # notifies, so a freshly-armed flow / arrived event is serviced at once.
        self._wake_cv = threading.Condition()
        self._wake_requested = False
        self._pre_round: Optional[Callable[[], None]] = None

        self._pump = threading.Thread(target=self._pump_loop,
                                      name="uf-pump", daemon=True)
        self._pump.start()

    # ----- attach / detach -----
    def _attach(self, m: Uniflow) -> None:
        with self._objects_mu:
            self._objects.append(m)

    def detach(self, m: Uniflow) -> None:
        with self._objects_mu:
            if m in self._objects:
                self._objects.remove(m)

    # ----- accessors -----
    @property
    def clock(self) -> VirtualClock:
        """This runtime's logical clock. rt.clock.SetScale(10) / .Freeze() /
        .Resume()."""
        return self._clock

    @property
    def observer(self) -> Observer:
        return self._observer

    @property
    def config(self) -> Config:
        return self._config

    def Post(self, fn: Callable[[], None]) -> None:
        """Run a callback on the pump thread (cross-thread state access without
        locks). Here it is forwarded onto the pump via the pre-round hook queue.
        Keep it short and non-blocking."""
        with self._posted_mu:
            self._posted.append(fn)
        self.Wake()

    def Wake(self) -> None:
        """Force the pump out of its inter-round nap now. Safe from any thread.
        StartTask / Post call it; call it yourself after mutating module state
        through your own channel so an event arrival is serviced immediately."""
        with self._wake_cv:
            self._wake_requested = True
            self._wake_cv.notify()

    def SetPreRound(self, fn: Optional[Callable[[], None]]) -> None:
        """Hook run once at the top of each round (e.g. a per-round snapshot).
        Exceptions are swallowed."""
        self._pre_round = fn

    def _drain_posted(self) -> bool:
        batch = []
        with self._posted_mu:
            if not self._posted:
                return False
            batch, self._posted = self._posted, []
        for fn in batch:
            try:
                fn()
            except Exception:  # noqa: BLE001
                pass
        return True

    def _pump_loop(self) -> None:
        while not self._stop.is_set():
            pre = self._pre_round
            if pre is not None:
                try:
                    pre()
                except Exception:  # noqa: BLE001
                    pass

            outcome = 0  # 0 = idle, 1 = staying, 2 = advanced
            if self._drain_posted():
                outcome = 2

            with self._objects_mu:
                objs = list(self._objects)
            for o in objs:
                if o.IsIdle():
                    continue
                if outcome == 0:
                    outcome = 1
                if o._execute_once(self._observer):
                    outcome = 2

            if outcome == 2:
                nap = self._config.step_interval_sleep_sec
            elif outcome == 1:
                nap = self._config.stay_sleep_sec
            else:
                nap = self._config.idle_sleep_sec
            if nap > 0:
                with self._wake_cv:
                    if not self._wake_requested:
                        self._wake_cv.wait(nap)
                    self._wake_requested = False

    def WaitUntilIdle(self, timeout: Optional[float] = None,
                      poll: float = 0.005) -> bool:
        """Block the calling thread until every module is idle. Never from the
        pump thread."""
        start = time.monotonic()
        while True:
            with self._objects_mu:
                busy = any(not o.IsIdle() for o in self._objects)
            if not busy:
                return True
            if timeout is not None and (time.monotonic() - start) >= timeout:
                return False
            time.sleep(poll)

    def CancelAll(self) -> None:
        with self._objects_mu:
            objs = list(self._objects)
        for o in objs:
            o.Cancel()

    def stop(self, join: bool = True, timeout: float = 2.0) -> None:
        self._stop.set()
        with self._wake_cv:
            self._wake_requested = True
            self._wake_cv.notify()
        if join and self._pump.is_alive():
            self._pump.join(timeout=timeout)
        self._executor.shutdown(wait=False, cancel_futures=True)

    def __enter__(self) -> "Runtime":
        return self

    def __exit__(self, *exc: Any) -> None:
        self.stop()
