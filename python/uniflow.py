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
    reason: str = ""
    # StayUntil 전용: step 진입 후 이 시간(초)이 지나면 next_fn 으로 전이.
    # 0 이면 일반 Stay (step 타임아웃 없음).
    timeout_sec: float = 0.0


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


class VirtualClock:
    """UFTimer 와 StayUntil 마감의 시간원. 기본은 실제 시계(time.monotonic)를 1:1 추종하되,
    set_scale 로 배속/감속, freeze/resume 으로 정지할 수 있다 — 시뮬 재생, e-stop/hold 중
    논리 타임아웃 오발 방지용. 논리 대기에만 적용되고 async/IO 데드라인·펌프 낮잠은 실제
    wall-clock 유지. now() 는 매 호출 fresh 계산(라운드 캐싱 없음)이라 타이머 기준이 무장한
    그 순간에 고정된다. thread-safe."""

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

    def now(self) -> float:
        with self._mu:
            return self._now_locked()

    def _rebase_locked(self) -> None:
        # 현재 가상시각을 새 원점으로 잡아 scale/freeze 변경이 불연속 점프 없게.
        self._base_virtual = self._now_locked()
        self._base_real = time.monotonic()

    def set_scale(self, scale: float) -> None:
        with self._mu:
            self._rebase_locked()
            self._scale = scale

    @property
    def scale(self) -> float:
        with self._mu:
            return self._scale

    def freeze(self) -> None:
        with self._mu:
            self._rebase_locked()
            self._frozen = True

    def resume(self) -> None:
        with self._mu:
            if self._frozen:
                self._base_real = time.monotonic()
                self._frozen = False

    @property
    def frozen(self) -> bool:
        with self._mu:
            return self._frozen


# 독립 UFTimer 의 기본 시간원 (scale 1, 절대 freeze 안 됨). Runtime 시계에 바인딩하면
# 그 런타임의 배속/freeze 를 따른다.
_REAL_CLOCK = VirtualClock()


def real_clock() -> VirtualClock:
    return _REAL_CLOCK


class UFTimer:
    """step 대기용 폴링 타이머. held_for 는 조건이 일정 시간 연속 충족됐는지(settling),
    passed 는 조건 없이 일정 시간 경과 여부를 본다. 기본은 실제 시계를 쓰고,
    UFTimer(rt.clock) 로 런타임 가상 시계(배속/freeze)에 바인딩할 수 있다."""

    def __init__(self, clock: Optional[VirtualClock] = None) -> None:
        self._clk = clock or _REAL_CLOCK
        self._armed_at = self._clk.now()
        self._cond_true = False

    def init(self) -> None:
        self._armed_at = self._clk.now()
        self._cond_true = False

    restart = init

    def elapsed(self) -> float:
        return self._clk.now() - self._armed_at

    def passed(self, duration: float) -> bool:
        return self.elapsed() >= duration

    def held_for(self, condition, duration: float) -> bool:
        # 조건이 duration 동안 "연속으로" 참이면 True. 중간에 한 번이라도
        # 거짓이 되면 누적이 리셋되고 False. 한 번 충족된 뒤에도 조건이
        # 유지되는 동안은 계속 True 를 돌려준다(레벨 의미).
        cond = condition() if callable(condition) else condition
        if not cond:
            self._cond_true = False
            return False
        if not self._cond_true:
            self._armed_at = self._clk.now()
            self._cond_true = True
        return (self._clk.now() - self._armed_at) >= duration


class Uniflow:
    def __init__(self, rt: "Runtime", *, name: Optional[str] = None) -> None:
        # name 은 키워드 전용 — Foo(rt, eq) 처럼 두 번째 위치인자를 넘기면 조용히
        # name 에 바인딩되지 않고 TypeError 가 난다.
        self._rt = rt
        self._name = name or type(self).__name__
        self._lock = threading.Lock()

        # 내장 타이머. step 진입(start, 매 Next)마다 자동 무장된다. step 경계를 넘는
        # 시간을 재려면 별도 UFTimer 를 만들어 쓴다. 런타임 가상 시계에 바인딩되어
        # rt.clock 배속/freeze 를 따른다.
        self.uf_timer = UFTimer(rt._clock)

        self._current: Optional[Callable[[], StepResult]] = None
        self._flow_running = False

        self._step_count = 0
        self._flow_t0 = 0.0
        self._step_t0 = 0.0      # 실제 시계 (프로파일링 elapsed_ms 용)
        self._step_t0_v = 0.0    # 가상 시계 (StayUntil 마감 용)
        self._step_ticks = 0
        self.failed = False
        self.fail_reason = ""
        self.fail_exc: Optional[BaseException] = None

        self._async: Optional[Future] = None
        self._async_t0 = 0.0
        self.async_value: Any = None

        rt._attach(self)

    def Stay(self) -> StepResult:
        # 같은 step 을 다음 라운드에 다시 실행. 폴링 간격은 Runtime 이 일괄 관리하는
        # stay_sleep 로 정해진다(step 별 gate 는 없음). 특정 시각까지 재우는 대신,
        # 조건이 만족될 때까지 Stay 로 폴링하고 외부 이벤트는 rt.wake() 로 깨운다.
        return StepResult(StepAction.STAY)

    def StayUntil(self, timeout_sec: float,
                  on_timeout: Callable[[], StepResult]) -> StepResult:
        # 현재 step 을 계속 폴링하되, step 진입 후 timeout_sec 이 지나면(이 호출
        # 시점이 아니라 step 진입 기준) on_timeout step 으로 전이한다. step 단위
        # catch: 도착하지 않을 수도 있는 조건을 무한 폴링하는 대신 정리/복구 경로로.
        return StepResult(StepAction.STAY,
                          next_fn=on_timeout,
                          next_name=getattr(on_timeout, "__name__", "?"),
                          timeout_sec=timeout_sec)

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
            # 잡이 끝나는 즉시 펌프를 깨워서, 다음 스텝의 완료 캐치가 stay_sleep
            # 주기(최악 20ms)만큼 늦어지지 않게 한다. 콜백은 워커 스레드에서 돌 수
            # 있으나 wake() 는 thread-safe.
            self._async.add_done_callback(lambda _f: self._rt.wake())
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
            self._step_count = 0
            self._step_ticks = 0
            self.failed = False
            self.fail_reason = ""
            self.fail_exc = None
            now = time.monotonic()
            self._flow_t0 = now
            self._step_t0 = now
            self._step_t0_v = self._rt._clock.now()
        self.uf_timer.init()
        self._rt._observer.on_flow_started(self._name, getattr(first_step, "__name__", "?"))
        # 펌프가 자고 있으면 즉시 깨워서 첫 step 이 nap 만큼 늦게 도는 것을 막는다.
        self._rt.wake()

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
            # StayUntil 마감 초과: next_fn(타임아웃 타깃)으로 Next 처럼 전이.
            # 마감은 런타임 가상 시계로 step 진입(self._step_t0_v) 기준이라 Stay 반복이
            # 시계를 안 밀고, 그 시계를 freeze/배속하면 마감도 같이 멈추거나 스케일된다.
            if result.timeout_sec > 0.0 and \
                    (self._rt._clock.now() - self._step_t0_v) >= result.timeout_sec:
                elapsed_ms = (time.monotonic() - self._step_t0) * 1000.0
                observer.on_step_changed(self._name, prev_name,
                                         result.next_name,
                                         elapsed_ms, self._step_ticks)
                with self._lock:
                    self._current = result.next_fn
                    self._step_count += 1
                    self._step_t0 = time.monotonic()
                    self._step_t0_v = self._rt._clock.now()
                    self._step_ticks = 0
                self.uf_timer.init()
                return True
            return False

        if result.action is StepAction.NEXT:
            elapsed_ms = (time.monotonic() - self._step_t0) * 1000.0
            observer.on_step_changed(self._name, prev_name, result.next_name,
                                     elapsed_ms, self._step_ticks)
            with self._lock:
                self._current = result.next_fn
                self._step_count += 1
                self._step_t0 = time.monotonic()
                self._step_t0_v = self._rt._clock.now()
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
        # 이 런타임 타이머/StayUntil 마감의 논리 시계. set_scale/freeze/resume 으로
        # 전체 흐름을 배속 재생하거나 정지. 모듈 내장 uf_timer 가 여기 바인딩된다.
        self._clock = VirtualClock()
        self._stop = threading.Event()
        # 인터럽트 가능한 라운드 간 대기. wake() 가 _wake_requested 를 세우고 cv 를
        # 깨우면, 갓 무장한 흐름/이벤트가 nap 을 다 채우지 않고 바로 처리된다.
        self._wake_cv = threading.Condition()
        self._wake_requested = False
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

    @property
    def clock(self) -> VirtualClock:
        """이 런타임의 논리 시계. rt.clock.set_scale(10) / .freeze() / .resume()."""
        return self._clock

    def submit(self, fn: Callable[[], Any]) -> Future:
        """블로킹 작업을 풀에 넘긴다 (보통 run_async 를 통해 쓴다)."""
        return self._executor.submit(fn)

    def wake(self) -> None:
        """펌프가 라운드 간 대기 중이면 즉시 깨운다. 어느 스레드에서나 호출 가능.
        start()/Post 가 자동으로 부르며, 외부 이벤트 스레드에서 모듈 상태를 바꾼 뒤
        직접 불러서 다음 폴링 주기를 기다리지 않고 바로 처리되게 할 수 있다."""
        with self._wake_cv:
            self._wake_requested = True
            self._wake_cv.notify()

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
                with self._wake_cv:
                    if not self._wake_requested:
                        self._wake_cv.wait(nap)
                    self._wake_requested = False

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
        # 펌프가 자고 있으면 깨워서 nap 을 다 기다리지 않고 즉시 종료하게 한다.
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
