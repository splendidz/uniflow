# uniflow.py 튜토리얼

> 🌐 언어: **한국어** | [English](TUTORIAL.md)

`uniflow.py` 의 짧고 Python다운 투어. 각 챕터는 작은 실행 가능한 프로그램입니다. Python 포트는 C++ 멘탈모델을 유지하되 기교(매크로/템플릿)는 버립니다 - step 은 그냥 `self.Next(...)` / `self.Stay()` / `self.Done()` / `self.Fail()` 을 반환하는 메서드입니다.

> API 레퍼런스: [PYTHON_PORT.md](PYTHON_PORT.md). 더 깊은 개념(단일 펌프 모델, observer 훅, 크로스-런타임)은 C++ 튜토리얼 [../cpp/TUTORIAL.md](../cpp/TUTORIAL.md) 가 같은 아이디어를 더 자세히 다룹니다.

모든 예제는 `uniflow.py` 가 import 가능하다고 가정합니다(표준 라이브러리만 쓰는 단일 파일):

```python
import uniflow as uf
```

---

## 챕터 1. 한 step짜리 모듈

가장 작은 모듈: `uf.Uniflow` 를 상속한 클래스 하나, step 메서드 하나, `Done()` 한 번.

```python
import uniflow as uf

class Hello(uf.Uniflow):
    def on_begin(self):
        print("hello from a step")
        return self.Done()

rt = uf.Runtime()
h = Hello(rt)
h.start(h.on_begin)        # on_begin 에서 flow 무장
rt.wait_until_idle()       # 모든 모듈이 idle 될 때까지 블록
rt.stop()
```

- `uf.Runtime()` 이 펌프 스레드 1개를 띄움.
- `Hello(rt)` 가 모듈을 attach; 펌프가 매 라운드 방문.
- `h.start(h.on_begin)` 이 flow 를 무장; step 은 다음 라운드에 실행.
- `self.Done()` 반환 시 모듈은 idle 로 복귀.

> step 본문에서 블로킹 `time.sleep(...)` 절대 금지 - 펌프 전체가 멈춥니다. `Stay()`(챕터 3) 또는 `run_async`(챕터 5)를 쓰세요.

---

## 챕터 2. step 체인 (Next)

실제 흐름은 `self.Next(self.다음메서드)` 로 N개를 잇습니다. 위에서 아래로 읽힙니다.

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

각 `Next` 는 다음 step 을 *다음 라운드* 에 예약합니다. step 경계가 바로 async 결과를 끼우는 자리입니다(챕터 5).

---

## 챕터 3. 폴링 (Stay) 과 타이머

`self.Stay()` 를 반환하면 다음 라운드에 같은 step 을 다시 실행 - 플래그 폴링이나 다른 모듈 대기용. 펌프는 라운드 사이 `stay_sleep`(기본 20ms) 쉽니다.

step 안에서는 `sleep` 을 절대 못 하므로 *시간* 은 폴링하는 타이머로 표현합니다. 모든 모듈엔 **start 와 매 `Next` 전이마다 자동 무장되는** 내장 `self.uf_timer` 가 있어서, 대기 step 에선 그냥 읽으면 됩니다:

```python
class WaitReady(uf.Uniflow):
    def on_begin(self):
        return self.Next(self.on_wait)
    def on_wait(self):
        # held_for: 조건이 0.05s 동안 연속으로 참이면 True (settling/디바운스).
        # passed(d): 조건 없이 d 가 경과하면 True.
        if self.uf_timer.held_for(hardware_ready, 0.05):
            return self.Next(self.on_go)
        return self.Stay()
    def on_go(self):
        return self.Done()
```

- `self.uf_timer.passed(d)` - step 진입 후 `d` 초가 지났나?
- `self.uf_timer.held_for(cond, d)` - `cond` 가 `d` 동안 연속으로 참이었나? (한 번이라도 false면 리셋)
- `self.uf_timer.elapsed()` - 원시 경과 초. 페이싱/진행률에.

step 을 넘나드는 타이머가 필요하면 직접 만드세요: 멤버로 `t = uf.UFTimer()`.

---

## 챕터 4. 모듈 여럿이 한 Runtime에서 (락 없음)

여러 모듈을 같은 `Runtime` 에 attach 하면 전부 같은 펌프 스레드에서 라운드로빈으로 돕니다. 모듈 간 공유 상태에 **락이 필요 없습니다** - 한 스레드니까.

```python
class Pinger(uf.Uniflow):
    def __init__(self, rt, box): super().__init__(rt); self.box = box; self.n = 3
    def on_begin(self): return self.Next(self.loop)
    def loop(self):
        if self.n <= 0: return self.Done()
        if self.box["turn"] != 0: return self.Stay()   # 내 차례 기다림
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
box = {"turn": 0}                 # 공유, 락 없음
p, q = Pinger(rt, box), Ponger(rt, box)
p.start(p.on_begin); q.start(q.on_begin)
rt.wait_until_idle(); rt.stop()
```

`box` 를 두 모듈에서 락 없이 만지는 이유: 둘 다 그 하나의 펌프 스레드에서 도니까요.

---

## 챕터 5. 블로킹 작업 - `run_async`

느린 함수를 step 에서 직접 부르면 펌프가 멈춥니다. `self.run_async(fn, then)` 으로 스레드풀에 넘기세요: 제출하고, 끝날 때까지 Stay 하다가, 결과를 `self.async_value` 에 담아 `then` 으로 진행합니다.

```python
import time

class Worker(uf.Uniflow):
    def on_begin(self):
        print("느린 잡 제출 (펌프는 안 막힘)")
        return self.run_async(self._slow, self.on_done)
    @staticmethod
    def _slow():
        time.sleep(0.5)            # 펌프가 아니라 풀 스레드에서 실행
        return 9 * 9
    def on_done(self):
        print("결과:", self.async_value)
        return self.Done()
```

잡이 도는 동안 펌프는 다른 모듈을 계속 돌립니다. 잡이 끝나면 펌프를 `wake()` 해서 `on_done` 이 결과를 바로 캐치합니다. `_slow` 가 예외를 던지면 흐름은 `Fail` 되고 예외가 보존됩니다.

> GIL 주의: I/O 바운드(네트워크/디스크)는 블로킹 중 GIL 을 풀어 실제 동시성을 얻습니다. CPU 바운드는 GIL 때문에 병렬화되지 않지만, offload 하면 펌프는 계속 반응합니다.

---

## 챕터 6. 스텝 타임아웃 - `StayUntil`

`run_async` 는 잡이 늦어진 경우를 처리합니다. 반대 경우 - *영영* 안 올 수도 있는 플래그 폴링 - 은 `self.StayUntil(timeout_sec, on_timeout)`: 이 step 을 계속 폴링하되, step 진입 후 `timeout_sec` 이 지나면 `on_timeout`(정리/복구 step)으로 빠집니다. step 단위 `catch` 라고 보면 됩니다.

```python
class Arm(uf.Uniflow):
    def on_begin(self): return self.Next(self.wait_ready)
    def wait_ready(self):
        if hardware_ready():
            return self.Next(self.on_go)
        return self.StayUntil(3.0, self.on_timeout)    # 3s 후 포기
    def on_go(self):
        return self.Done()
    def on_timeout(self):
        print("hw 가 끝내 ready 안 됨 - 중단")
        return self.Fail("timeout")
```

마감은 step 진입 기준이라 반복되는 Stay 틱이 시계를 뒤로 밀지 않습니다.

> **가상 시계.** 타이머와 `StayUntil` 마감은 `rt.clock` 위에서 돕니다 - 배속/정지 가능한 시계: `rt.clock.set_scale(10)` 은 전체 흐름을 10배속 재생, `rt.clock.freeze()` / `.resume()` 은 모든 논리 타임아웃을 정지(예: e-stop 중 3s 타임아웃이 멈춤 동안 안 터지게). async/IO 데드라인은 실제 시간 유지.

---

## 챕터 7. Observer

모든 이벤트(flow/step/async/예외/종료)는 `Observer` 로 흐릅니다. 기본 `ConsoleObserver` 는 stdout 에 예쁘게 찍습니다. 조용히 하거나 커스텀 로깅하려면 상속해서 넘기세요:

```python
class Quiet(uf.Observer):
    pass                                   # 아무것도 override 안 함 -> 출력 없음

class MyObs(uf.Observer):
    def on_step_changed(self, obj, prev, nxt, elapsed_ms, ticks):
        print(f"{obj}: {prev} -> {nxt} ({elapsed_ms:.1f}ms)")
    def on_flow_ended(self, obj, action, steps, wall_ms, reason):
        if action is uf.StepAction.FAIL:
            print(f"{obj} FAILED: {reason}")

rt = uf.Runtime(observer=MyObs())          # rt 의 모든 모듈이 이걸 씀
```

---

## 챕터 8. 외부에서 구동하기, 그리고 생명주기

흐름은 보통 펌프가 *아닌* 무언가가 시작합니다 - 이벤트 스레드, 소켓 콜백, main 스레드.

```python
# 어느 스레드에서나: 흐름 시작은 안전하고 펌프를 즉시 깨우므로,
# 첫 step 이 다음 20ms 폴링이 아니라 지금 돕니다.
def on_message(msg):
    handler.start(lambda: handler.on_handle(msg))

# 자기 채널로 상태를 바꾼 뒤 펌프를 직접 깨울 수도 있습니다:
rt.wake()
```

생명주기 제어:
- `module.is_idle()` - 비었나? 오케스트레이터가 다른 모듈에 일 주기 전에 확인.
- `rt.wait_until_idle(timeout=None)` - 모든 모듈이 idle 될 때까지 호출 스레드 블록(`main` 이 종료 전 기다리는 법). step 안에서는 부르지 마세요.
- `module.cancel()` - 도는 흐름을 협력적으로 종료(이유 `"cancelled"` 로 fail 표시); `rt.cancel_all()` 은 전체에.
- `rt.stop()` - 펌프 정지 + 풀 종료.

---

## 다음

- [PYTHON_PORT.md](PYTHON_PORT.md) - 전체 API 표와 설계 결정.
- [../cpp/TUTORIAL.md](../cpp/TUTORIAL.md) - C++ 튜토리얼; 같은 개념을 단일 펌프 모델·크로스-런타임까지 더 깊이.
- [../README.md](../README.md) - 프로젝트 개요.
