"""uniflow.py - 단일 스레드 협력적 step 기반 비동기 프레임워크.

모듈은 Uniflow 를 상속하고 로직을 StepResult(Stay/Next/Done/Fail)를 반환하는 멤버 함수
체인으로 쓴다. 하나의 Runtime 펌프 스레드가 attach 된 모든 모듈을 라운드로빈으로 돌려서
여러 모듈이 한 스레드에서 락 없이 진행된다. 블로킹 작업은 run_async 로 풀에 넘긴다.
"""

from __future__ import annotations

import threading
import time
from concurrent.futures import Future, ThreadPoolExecutor
from dataclasses import dataclass
from enum import Enum
from typing import Any, Callable, List, Optional


class StepAction(Enum):
    STAY = "Stay"
    NEXT = "Next"
    DONE = "Done"
    FAIL = "Fail"


@dataclass
class StepResult:
    action: StepAction
    next_fn: Optional[Callable[[], "StepResult"]] = None
    next_name: str = ""
    gate_sec: float = 0.0
    reason: str = ""


# 프레임워크 이벤트 훅. 서브클래싱해서 필요한 것만 override. 펌프 스레드에서 호출된다.
class Observer:
    def on_flow_started(self, obj: str, first_step: str) -> None: ...
    def on_step_changed(self, obj: str, prev_step: str, next_step: str,
                        elapsed_ms: float, ticks: int) -> None: ...
    def on_step_threw(self, obj: str, step: str, what: str) -> None: ...
    def on_async_submitted(self, obj: str, step: str) -> None: ...
    def on_async_completed(self, obj: str, wait_ms: float, had_error: bool) -> None: ...
    def on_flow_ended(self, obj: str, action: StepAction, steps: int,
                      wall_ms: float, reason: str) -> None: ...


class ConsoleObserver(Observer):
    """이벤트를 stdout 에 한 줄씩 출력. thread-safe."""

    def __init__(self) -> None:
        self._mu = threading.Lock()

    def _print(self, line: str) -> None:
        with self._mu:
            print(line, flush=True)

    def on_flow_started(self, obj, first_step):
        self._print(f"[{obj:<16}] FLOW START  -> {first_step}")

    def on_step_changed(self, obj, prev_step, next_step, elapsed_ms, ticks):
        arrow = f"{prev_step} -> {next_step}" if next_step else prev_step
        self._print(f"[{obj:<16}] {arrow:<40} elapsed={elapsed_ms:.2f}ms tick x{ticks}")

    def on_step_threw(self, obj, step, what):
        self._print(f"[{obj:<16}] {step:<40} [THREW] {what}")

    def on_async_submitted(self, obj, step):
        self._print(f"[{obj:<16}] {step:<40} ASYNC SUBMIT")

    def on_async_completed(self, obj, wait_ms, had_error):
        tag = " [ERROR]" if had_error else ""
        self._print(f"[{obj:<16}] {'':<40} ASYNC DONE  wait={wait_ms:.2f}ms{tag}")

    def on_flow_ended(self, obj, action, steps, wall_ms, reason):
        extra = f"  reason={reason}" if reason else ""
        self._print(f"[{obj:<16}] FLOW END  {action.value:<5} steps=#{steps:02d} "
                    f"wall={wall_ms:.2f}ms{extra}")


class _SafeObserver(Observer):
    """observer 예외가 펌프 스레드를 죽이지 않게 감싼다. Runtime 이 모든 observer 를 이걸로 감싼다."""
    def __init__(self, inner: Observer) -> None:
        self._inner = inner

    def on_flow_started(self, *a):
        try: self._inner.on_flow_started(*a)
        except Exception: pass

    def on_step_changed(self, *a):
        try: self._inner.on_step_changed(*a)
        except Exception: pass

    def on_step_threw(self, *a):
        try: self._inner.on_step_threw(*a)
        except Exception: pass

    def on_async_submitted(self, *a):
        try: self._inner.on_async_submitted(*a)
        except Exception: pass

    def on_async_completed(self, *a):
        try: self._inner.on_async_completed(*a)
        except Exception: pass

    def on_flow_ended(self, *a):
        try: self._inner.on_flow_ended(*a)
        except Exception: pass


class TimerResult(Enum):
    WAITING = "Waiting"
    DONE = "Done"
    TIMEOUT = "Timeout"


class UFTimer:
    """step 대기용 폴링 타이머. on_wait 는 timeout 안에 조건 충족 여부를,
    on_delay 는 조건 없이 일정 시간 경과 여부를 본다."""

    def __init__(self) -> None:
        self._armed_at = time.monotonic()

    def init(self) -> None:
        self._armed_at = time.monotonic()

    restart = init

    def elapsed(self) -> float:
        return time.monotonic() - self._armed_at

    def timed_out(self, timeout: float) -> bool:
        return self.elapsed() >= timeout

    def on_wait(self, condition, timeout: float) -> TimerResult:
        cond = condition() if callable(condition) else condition
        if cond:
            return TimerResult.DONE
        if self.elapsed() >= timeout:
            return TimerResult.TIMEOUT
        return TimerResult.WAITING

    def on_delay(self, seconds: float) -> TimerResult:
        return TimerResult.DONE if self.elapsed() >= seconds else TimerResult.WAITING


class Uniflow:
    def __init__(self, rt: "Runtime", *, name: Optional[str] = None) -> None:
        # name 은 키워드 전용 — Foo(rt, eq) 처럼 두 번째 위치인자를 넘기면 조용히
        # name 에 바인딩되지 않고 TypeError 가 난다.
        self._rt = rt
        self._name = name or type(self).__name__
        self._lock = threading.Lock()

        # 내장 타이머. step 진입(start, 매 Next)마다 자동 무장된다. step 경계를 넘는
        # 시간을 재려면 별도 UFTimer 를 만들어 쓴다.
        self.uf_timer = UFTimer()

        self._current: Optional[Callable[[], StepResult]] = None
        self._flow_running = False
        self._wake_at = 0.0

        self._step_count = 0
        self._flow_t0 = 0.0
        self._step_t0 = 0.0
        self._step_ticks = 0
        self.failed = False
        self.fail_reason = ""
        self.fail_exc: Optional[BaseException] = None

        self._async: Optional[Future] = None
        self._async_t0 = 0.0
        self.async_value: Any = None

        rt._attach(self)

    def Stay(self, gate_sec: float = 0.0) -> StepResult:
        return StepResult(StepAction.STAY, gate_sec=gate_sec)

    def Next(self, fn: Callable[[], StepResult]) -> StepResult:
        return StepResult(StepAction.NEXT, next_fn=fn,
                          next_name=getattr(fn, "__name__", "?"))

    def Done(self) -> StepResult:
        return StepResult(StepAction.DONE)

    def Fail(self, reason: str = "") -> StepResult:
        return StepResult(StepAction.FAIL, reason=reason)

    # 블로킹 fn 을 풀에 넘기고 끝날 때까지 Stay, 완료되면 Next(then). 결과는
    # self.async_value, 예외가 나면 Fail.
    def run_async(self, fn: Callable[[], Any],
                  then: Callable[[], StepResult]) -> StepResult:
        if self._async is None:
            self._async = self._rt._executor.submit(fn)
            self._async_t0 = time.monotonic()
            self._rt._observer.on_async_submitted(self._name, self._cur_name())
            return self.Stay()
        if not self._async.done():
            return self.Stay()
        fut = self._async
        self._async = None
        wait_ms = (time.monotonic() - self._async_t0) * 1000.0
        exc = fut.exception()
        self._rt._observer.on_async_completed(self._name, wait_ms, exc is not None)
        if exc is not None:
            self.fail_exc = exc
            return self.Fail(f"async error: {exc!r}")
        self.async_value = fut.result()
        return self.Next(then)

    def start(self, first_step: Callable[[], StepResult]) -> None:
        """흐름을 시작한다. first_step 은 bound method (예: self.on_begin)."""
        with self._lock:
            self._current = first_step
            self._flow_running = True
            self._wake_at = 0.0
            self._step_count = 0
            self._step_ticks = 0
            self.failed = False
            self.fail_reason = ""
            self.fail_exc = None
            now = time.monotonic()
            self._flow_t0 = now
            self._step_t0 = now
        self.uf_timer.init()
        self._rt._observer.on_flow_started(self._name, getattr(first_step, "__name__", "?"))

    def cancel(self) -> None:
        """흐름을 즉시 Fail 로 멈춘다. 펌프/외부 스레드 모두 호출 가능."""
        with self._lock:
            if not self._flow_running:
                return
            self._flow_running = False
            self.failed = True
            self.fail_reason = "cancelled"

    @property
    def name(self) -> str:
        return self._name

    def is_idle(self) -> bool:
        with self._lock:
            return not self._flow_running

    def _cur_name(self) -> str:
        cur = self._current
        return getattr(cur, "__name__", "?") if cur else "?"

    # 펌프가 한 tick 호출. 전이(Next/Done/Fail)면 True.
    def _execute_once(self, observer: Observer) -> bool:
        with self._lock:
            if not self._flow_running:
                return False
            if self._wake_at and time.monotonic() < self._wake_at:
                return False
            self._wake_at = 0.0
            step = self._current
        if step is None:
            return False

        prev_name = getattr(step, "__name__", "?")
        self._step_ticks += 1
        try:
            result = step()
        except BaseException as e:  # noqa: BLE001
            observer.on_step_threw(self._name, prev_name, repr(e))
            self._end_flow(StepAction.FAIL, observer, reason=f"step threw: {e!r}", exc=e)
            return True

        if result.action is StepAction.STAY:
            if result.gate_sec > 0.0:
                with self._lock:
                    self._wake_at = time.monotonic() + result.gate_sec
            return False

        if result.action is StepAction.NEXT:
            elapsed_ms = (time.monotonic() - self._step_t0) * 1000.0
            observer.on_step_changed(self._name, prev_name, result.next_name,
                                     elapsed_ms, self._step_ticks)
            with self._lock:
                self._current = result.next_fn
                self._step_count += 1
                self._step_t0 = time.monotonic()
                self._step_ticks = 0
            self.uf_timer.init()
            return True

        self._end_flow(result.action, observer, reason=result.reason)
        return True

    def _end_flow(self, action: StepAction, observer: Observer,
                  reason: str = "", exc: Optional[BaseException] = None) -> None:
        with self._lock:
            self._flow_running = False
            if action is StepAction.FAIL:
                self.failed = True
                self.fail_reason = reason
                if exc is not None:
                    self.fail_exc = exc
            wall_ms = (time.monotonic() - self._flow_t0) * 1000.0
            steps = self._step_count
        observer.on_flow_ended(self._name, action, steps, wall_ms, reason)


# 펌프 스레드 1개 + executor + observer. 생성하면 펌프가 돌기 시작하고, attach 된
# 모듈을 매 라운드 한 tick 씩 라운드로빈 구동한다.
class Runtime:
    def __init__(self, *, threads: int = 4,
                 observer: Optional[Observer] = None,
                 idle_sleep: float = 0.001,   # 아무 흐름도 안 돌 때
                 stay_sleep: float = 0.02,    # 모두 Stay 일 때 폴링 간격
                 step_sleep: float = 0.0,     # 전이 직후 (0=버스트)
                 ) -> None:
        self._objects: List[Uniflow] = []
        self._objects_mu = threading.Lock()
        self._executor = ThreadPoolExecutor(max_workers=max(1, threads),
                                            thread_name_prefix="uf-pool")
        self._observer = _SafeObserver(observer or ConsoleObserver())
        self._idle_sleep = idle_sleep
        self._stay_sleep = stay_sleep
        self._step_sleep = step_sleep
        self._stop = threading.Event()
        self._pre_round: Optional[Callable[[], None]] = None
        self._pump = threading.Thread(target=self._pump_loop,
                                      name="uf-pump", daemon=True)
        self._pump.start()

    def _attach(self, m: Uniflow) -> None:
        with self._objects_mu:
            self._objects.append(m)

    def detach(self, m: Uniflow) -> None:
        with self._objects_mu:
            if m in self._objects:
                self._objects.remove(m)

    @property
    def observer(self) -> Observer:
        return self._observer

    def submit(self, fn: Callable[[], Any]) -> Future:
        """블로킹 작업을 풀에 넘긴다 (보통 run_async 를 통해 쓴다)."""
        return self._executor.submit(fn)

    def set_pre_round(self, fn: Optional[Callable[[], None]]) -> None:
        """매 라운드 시작 직전에 호출할 훅 (예: 라운드당 1회 상태 갱신). 예외는 무시된다."""
        self._pre_round = fn

    def _pump_loop(self) -> None:
        while not self._stop.is_set():
            pre = self._pre_round
            if pre is not None:
                try:
                    pre()
                except Exception:  # noqa: BLE001
                    pass

            outcome = 0  # 0=idle, 1=staying, 2=advanced
            with self._objects_mu:
                objs = list(self._objects)
            for o in objs:
                if o.is_idle():
                    continue
                if outcome == 0:
                    outcome = 1
                if o._execute_once(self._observer):
                    outcome = 2

            if outcome == 2:
                nap = self._step_sleep
            elif outcome == 1:
                nap = self._stay_sleep
            else:
                nap = self._idle_sleep
            if nap > 0:
                self._stop.wait(nap)

    def wait_until_idle(self, timeout: Optional[float] = None,
                        poll: float = 0.01) -> bool:
        """모든 모듈이 idle 이 될 때까지 호출 스레드를 블록. 펌프 스레드에서 부르면 안 됨."""
        start = time.monotonic()
        while True:
            with self._objects_mu:
                busy = any(not o.is_idle() for o in self._objects)
            if not busy:
                return True
            if timeout is not None and (time.monotonic() - start) >= timeout:
                return False
            time.sleep(poll)

    def cancel_all(self) -> None:
        """모든 흐름을 협력적으로 취소(Fail)."""
        with self._objects_mu:
            objs = list(self._objects)
        for o in objs:
            o.cancel()

    def any_failed(self) -> List[Uniflow]:
        """Fail 로 끝난 모듈 목록."""
        with self._objects_mu:
            return [o for o in self._objects if o.failed]

    def stop(self, join: bool = True, timeout: float = 2.0) -> None:
        self._stop.set()
        if join and self._pump.is_alive():
            self._pump.join(timeout=timeout)
        self._executor.shutdown(wait=False, cancel_futures=True)

    def __enter__(self) -> "Runtime":
        return self

    def __exit__(self, *exc: Any) -> None:
        self.stop()
