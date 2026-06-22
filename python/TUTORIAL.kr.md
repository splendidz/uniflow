# uniflow.py 튜토리얼

> Language: **한국어** | [English](TUTORIAL.md)

`uniflow.py` 의 Python다운 투어. 각 챕터는 작은 실행 가능한 프로그램이다. Python 포트는 C++ 멘탈모델을 유지하고 공개 이름까지 그대로 미러링하되(Task-Level Syntax) 기교(매크로/템플릿)는 버린다. **모듈** 은 `uniflow.Uniflow` 를 상속해 하나 이상의 **task** 를 소유하고, **task** 는 `uniflow.Task` 를 상속해 자기 step 메서드를 소유한다. step 은 의도를 반환하는 메서드다 - `self.Next(...)` / `self.Stay()` / `self.Done()` / `self.Fail()`.

> API 레퍼런스: [PYTHON_PORT.md](PYTHON_PORT.md). 더 깊은 개념(단일 펌프 모델, observer 훅, 크로스-런타임)은 C++ 튜토리얼 [../cpp/TUTORIAL.md](../cpp/TUTORIAL.md) 가 같은 아이디어를 더 자세히 다룬다.

모든 예제는 `uniflow.py` 가 import 가능하다고 가정한다(표준 라이브러리만 쓰는 단일 파일):

```python
import uniflow
```

> **예제.** 여기서 언급하는 여섯 개의 예제는 [python/examples](examples/) 에 있다 - `simulator.py`, `shared_ostream.py`, `message_dispatch.py`, `pick_and_place.py`, `queue_drain.py`, `city_traffic.py`. [../cpp/examples](../cpp/examples/) 의 C++ 세트, [../cs/examples](../cs/examples/) 의 C# 세트와 짝을 이루므로 같은 프로그램이 세 언어에서 똑같이 읽힌다.

> **단위.** Python 포트의 시간 단위는 **초** 다(C++ 포트는 밀리초). `rt.clock.Now()`, `UFTimer`, `StayUntil` 마감이 모두 초 단위다.

---

## 챕터 1. 모듈, task, 한 개의 step

가장 작은 단위는 `uniflow.Uniflow` 를 상속하고 task 하나를 소유하는 모듈이다. task 는 `uniflow.Task` 를 상속해 `Entry()` 로 첫 step 을 지정하고, step 은 `Done()` 을 반환한다.

```python
import uniflow

class Flow_Hello(uniflow.Uniflow):
    def __init__(self, rt):
        super().__init__(rt, name="Flow_Hello")
        self.ctx = self.Task_Hello()      # 모듈의 단일 task
        self.AddTask(self.ctx)            # flow() 역참조 연결; 아무것도 시작 안 함

    class Task_Hello(uniflow.Task):
        def Entry(self):                  # 첫 step 지정
            return self.Step1_Greet()

        def Step1_Greet(self):
            print("hello from a step")
            return self.Done()

rt = uniflow.Runtime()
h = Flow_Hello(rt)
h.ctx.StartFlow()             # task 런칭; 첫 step 은 다음 라운드에 실행
rt.WaitUntilIdle()            # 모든 모듈이 idle 될 때까지 블록
rt.stop()
```

- `uniflow.Runtime()` 이 펌프 스레드 1개를 띄움.
- `Flow_Hello(rt)` 가 모듈을 attach; 펌프가 매 라운드 방문.
- `AddTask(self.ctx)` 가 task 를 바인딩해서 그 step 들이 `self.flow()` 로 모듈에 닿게 함.
- `h.ctx.StartFlow()` 가 task 를 런칭; `Entry()` 가 다음 라운드에 실행. 반환값은 `StartResult.Ok`, 이미 task 가 돌고 있으면 `StartResult.Busy`.
- `self.Done()` 반환 시 모듈은 idle 로 복귀.

> step 본문에서 블로킹 `time.sleep(...)` 은 금지한다 - 펌프 전체가 멈춘다. `Stay()`(챕터 3) 또는 `SubmitAsync`(챕터 5)를 사용한다.

---

## 챕터 2. step 체인 (Next)

실제 task 는 `self.Next(self.다음step)` 으로 N개를 잇는다. step 들은 같은 task 의 형제 메서드이고, 위에서 아래로 읽힌다.

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

각 `Next` 는 다음 step 을 *다음 라운드* 에 예약한다. `Next` 는 현재 task 를 벗어나지 않고 형제 step 으로만 진행한다. step 경계가 async 결과를 끼우는 자리다(챕터 5).

인자도 넘길 수 있다: `self.Next(self.Step2_Wait, job_id)` 는 다음 라운드에 `Step2_Wait(self, job_id)` 를 호출한다 - 제출 step 에서 폴링 step 으로 `AsyncId` 를 넘기는 정석이다.

---

## 챕터 3. 폴링 (Stay) 과 타이머

`self.Stay()` 를 반환하면 다음 라운드에 같은 step 을 다시 실행한다 - 플래그 폴링이나 다른 모듈 대기용. 펌프는 모두-Stay 라운드 사이에 `stay_sleep`(기본 20ms) 쉰다.

step 안에서는 `sleep` 을 할 수 없으므로 *시간* 은 폴링하는 타이머로 표현한다. 모든 모듈은 (런타임 시계에 바인딩되어) **매 `Next` 전이마다 재무장되는** 내장 `self.uf_timer` 를 가지며, 대기 step 에선 `self.flow()` 로 모듈에서 읽는다. 한 task 안에서만 쓰는 타이머는 직접 소유하고 `OnEnter()` 에서 무장하는 편이 깔끔하다:

```python
import uniflow

def hardware_ready():
    ...   # 센서/플래그 읽기

class Flow_WaitReady(uniflow.Uniflow):
    def __init__(self, rt):
        super().__init__(rt, name="Flow_WaitReady")
        self.ctx = self.Task_Wait()
        self.AddTask(self.ctx)

    class Task_Wait(uniflow.Task):
        def OnEnter(self):
            # 진입 시 per-task 상태 재무장; rt.clock(배속/정지) 에 바인딩
            self.settle = uniflow.UFTimer(self.flow()._rt.clock)

        def Entry(self):
            return self.Step1_Wait()

        def Step1_Wait(self):
            # HeldFor: 조건이 0.05s 동안 연속으로 참이면 True (settling/디바운스).
            # 한 번이라도 false면 누적이 리셋된다.
            if self.settle.HeldFor(hardware_ready, 0.05):
                return self.Next(self.Step2_Go)
            return self.Stay()

        def Step2_Go(self):
            return self.Done()
```

- `timer.Passed(d)` - 타이머 무장 후 `d` 초가 지났나?
- `timer.HeldFor(cond, d)` - `cond` 가 `d` 동안 연속으로 참이었나? (한 번이라도 false면 리셋)
- `timer.Elapsed()` - 원시 경과 초. 페이싱/진행률에.

`OnEnter()` 는 task 가 진입될 때마다 첫 step 직전 1회 실행된다 - per-task 상태 재무장 자리. `Entry()` 는 override 해서 첫 step 을 지정한다.

---

## 챕터 4. 모듈 여럿이 한 Runtime에서 (락 없음)

여러 모듈을 같은 `Runtime` 에 attach 하면 전부 같은 펌프 스레드에서 라운드로빈으로 돈다. 모듈 간 공유 상태에 **락이 필요 없다** - 한 스레드이기 때문이다. `shared_ostream.py` 예제의 축소판이다.

```python
import uniflow

class SharedState:                # sink 하나 + turn 플래그, 펌프 스레드만 건드림
    log = []
    turn = 0

class Flow_Writer(uniflow.Uniflow):
    def __init__(self, rt, text, count, turn_id):
        super().__init__(rt, name="Flow_Writer")
        self.text = text
        self.remaining = count
        self.turn_id = turn_id        # 0 또는 1; 내 차례에만 쓴다
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
                return self.Stay()            # 내 차례 아님 - 다음 라운드 재폴링
            SharedState.log.append(f.text)    # 공유 sink, 락 없음
            SharedState.turn = 1 - SharedState.turn
            f.remaining -= 1
            return self.Stay()

rt = uniflow.Runtime(observer=uniflow.Observer())     # 조용한 observer
hello = Flow_Writer(rt, "Hello ", 3, 0)
world = Flow_Writer(rt, "World. ", 3, 1)
hello.ctx.StartFlow(); world.ctx.StartFlow()
hello.WaitUntilIdle(); world.WaitUntilIdle(); rt.stop()
print("".join(SharedState.log))    # Hello World. Hello World. Hello World.
```

`SharedState` 를 두 모듈에서 락 없이 만질 수 있는 것은 둘 다 그 하나의 펌프 스레드에서 돌기 때문이다. `self.flow()` 는 task step 안에서 소유 모듈에 닿는다.

> `observer=uniflow.Observer()` 를 넘기면 **조용한** observer 가 설치된다(베이스 클래스가 no-op). 기본값은 모든 이벤트를 찍는 `ConsoleObserver` 로, 배우는 동안엔 유용하지만 프로그램이 stdout 을 소유하면 끈다. 챕터 7 참고.

---

## 챕터 5. 블로킹 작업 - `SubmitAsync` + `AsyncResult`

느린 함수를 step 에서 직접 부르면 펌프가 멈춘다. `self.SubmitAsync(...)` 로 스레드풀에 넘기면 **AsyncId** 를 즉시 돌려받는다. 그 id 를 뒤 step 으로 넘겨 `self.AsyncResult(id)` 로 **폴링** 한다. 펌프는 절대 막히지 않는다.

```python
import time
import uniflow

def slow_square(n):
    time.sleep(0.5)            # 펌프가 아니라 풀 스레드에서 실행
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
            print("느린 잡 제출 (펌프는 안 막힘)")
            # (fn, label, timeout_sec, *args); timeout_sec=None 이면 타임아웃 없음.
            # fn 은 모듈 레벨/정적 함수 - task 에 접근하지 못한다.
            aid = self.SubmitAsync(slow_square, "slow_square", None, 9)
            if aid == 0:
                return self.Fail(reason="rejected: in-flight cap reached")
            return self.Next(self.Step2_Wait, aid)     # id 를 앞으로 넘김

        def Step2_Wait(self, aid):
            r = self.AsyncResult(aid)      # AsyncOutcome 스냅샷
            if r.pending():
                return self.Stay()         # 아직 진행 중 - 재폴링
            if not r.ok():
                return self.Fail(reason="job failed or timed out")
            print("결과:", r.return_value)
            return self.Done()
```

잡이 도는 동안 펌프는 다른 모듈을 계속 돌린다. 잡이 끝나면 펌프를 깨우므로, 다음 `Step2_Wait` 폴링이 `stay_sleep` 만큼 기다리지 않고 결과를 바로 캐치한다.

`AsyncResult(id)` 는 `.state` 와 다음 술어를 가진 `AsyncOutcome` 을 반환한다: `.ok()`(Done - `.return_value` 유효), `.pending()`(진행 중), `.failed()`(워커가 예외), `.is_timeout()`(마감 초과), `.found()`(id 가 살아있는 슬롯에 매칭). 잘못된/지워진 id(`0` 포함)는 `NotFound` 로 읽힌다. 모듈은 `self.AnyAsyncPending()` 과 `self.ClearAsync()`(모든 진행 중 워커 폐기)도 제공한다.

잡에 마감을 주려면 `timeout_sec`(실제 초, 3번째 인자)을 넘긴다: `self.SubmitAsync(fn, "label", 2.0, *args)`. 마감 후 결과는 `is_timeout()` 으로 읽히고 워커는 폐기된다(버려진 결과로 계속 돌긴 함).

> GIL 주의: I/O 바운드(네트워크/디스크)는 블로킹 중 GIL 을 풀어 실제 동시성을 얻는다. CPU 바운드는 GIL 때문에 병렬화되지 않지만, offload 하면 펌프는 계속 반응한다.

---

## 챕터 6. 스텝 타임아웃 - `StayUntil`

`SubmitAsync` 는 *잡* 이 늦어진 경우를 처리한다. 한편 잡을 기다리는 게 아닌 경우도 있다 - 하드웨어에 명령을 내려놓고 "완료" 플래그나 센서를 `Stay()` 로 폴링하는 경우로, 신호가 끝내 오지 않을 수 있다. 축이 끼이거나 밸브가 멈추면 맨 `Stay()` 루프는 무한히 폴링하고, 라인은 에러도 없이 멈춰 선다. 실제 장비에선 심각한 고장 양상이다.

`self.StayUntil(timeout_sec, on_timeout)` 은 **마감이 달린 `Stay()`** 다: 이 step 을 계속 폴링하되, step 에 *진입한 시점* 부터 **논리 시간** 으로 `timeout_sec` 이 지나면 `on_timeout` 으로 빠진다. 그 step 이 `catch` 역할을 한다 - 정해진 복구 경로로의 보장된 탈출구다.

전체 패턴 - 명령, 마감 걸린 대기, 복구:

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
            f.axis.move_to(f.target)              # 명령 발사 (논블로킹)
            return self.Next(self.Step2_WaitInPos)

        def Step2_WaitInPos(self):
            if self.flow().axis.in_position():    # 정상 경로
                return self.Next(self.Step3_Clamp)
            # 아직 이동 중 - 폴링하되 2s 넘게 멈춰있으면 포기
            return self.StayUntil(2.0, self.Step_Stalled)

        # 대기 step 진입 후 2s 안에 in_position 이 끝내 true 가 안 된 경우에만 도달.
        # 흐름은 멈춰 설 수 없다 - 항상 정의된 어딘가로 도착한다.
        def Step_Stalled(self):
            self.flow().axis.abort()
            print("axis stalled before reaching target")
            return self.Fail(reason="stalled")

        def Step3_Clamp(self):
            return self.Done()
```

`StayUntil` 없이 `Step2_WaitInPos` 가 맨 `Stay()` 를 반환했다면, 누군가 라인이 죽은 걸 알아챌 때까지 무한히 돈다. 이걸 쓰면 이동이 안 끝날 경우 `Step_Stalled` 에 반드시 도달한다.

복구 step 도 하나의 step 이므로 어디로든 라우팅할 수 있다. 흔한 형태가 *재시도 후 포기* 다:

```python
        def Step2_WaitInPos(self):
            if self.flow().axis.in_position():
                return self.Next(self.Step3_Clamp)
            return self.StayUntil(2.0, self.Step_Retry)

        def Step_Retry(self):
            f = self.flow()
            f.tries += 1
            if f.tries >= 3:
                print("axis failed after 3 tries")
                return self.Fail(reason="gave up")
            f.axis.abort()
            return self.Next(self.Step1_Command)   # 재발행 -> 대기 step 재진입,
                                                   # 2s 창이 다시 시작됨
```

`Step1_Command` -> `Step2_WaitInPos` 재진입은 새 step 진입이므로, 시도마다 2s 마감이 새로 시작된다 - 수동 타이머 관리가 필요 없다. 마감은 step 진입 기준이라 반복되는 Stay 틱이 시계를 뒤로 밀지 않는다.

> **가상 시계.** 타이머와 `StayUntil` 마감은 `rt.clock` 위에서 돈다 - 배속/정지 가능한 시계다. `rt.clock.SetScale(10)` 은 전체 흐름을 10배속 재생하고, `rt.clock.Freeze()` / `.Resume()` 은 모든 논리 타임아웃을 정지한다(예: e-stop 중 2s 타임아웃이 멈춤 동안 안 터지게). async/IO 데드라인은 실제 시간을 유지한다. `simulator.py` 예제가 이 전부를 키보드로 실시간 구동한다.

---

## 챕터 7. Observer

모든 이벤트(flow/step/async/예외/종료)는 `Observer` 로 흐른다. 기본 `ConsoleObserver` 는 stdout 에 찍는다. 조용히 하거나 커스텀 로깅하려면 상속해서 넘긴다:

```python
import uniflow

class Flow_Observed(uniflow.Observer):   # 빈 Observer -> 출력 없음 (조용)
    pass

class MyObserver(uniflow.Observer):
    def OnStepChanged(self, obj, prev_step, next_step, description,
                      step_ordinal, elapsed_ms, ticks):
        print(f"{obj}: {prev_step} -> {next_step}  {description} ({elapsed_ms:.1f}ms)")

    def OnFlowEnded(self, obj, terminal_action, final_step_ordinal, wall_ms, reason):
        if terminal_action is uniflow.StepAction.FAIL:
            print(f"{obj} FAILED: {reason}")

rt = uniflow.Runtime(observer=MyObserver())      # rt 의 모든 모듈이 이걸 씀
```

`description` 은 step 이 `self.Describe(...)` 로 적은 한 줄로, "지금 뭘 하는지" 를 로그에 남기는 용도다. step 전환 시 한 번 찍히고 비워진다:

```python
        def Step2_WaitInPos(self):
            self.Describe("approaching ", self.flow().target, " mm")   # OnStepChanged 에 나타남
            if self.flow().axis.in_position():
                return self.Next(self.Step3_Clamp)
            return self.Stay()
```

`module.CurrentStepDescription()` 로 실시간 조회도 된다 (`CurrentStepName()` / `CurrentStepOrdinal()` 로 흐름이 지금 어디인지도). 렌더러 flow 가 이 읽기를 사용한다 - `simulator.py` 와 `message_dispatch.py` 참고.

> 전체 observer 표면은 [PYTHON_PORT.md](PYTHON_PORT.md) 에 있다: `OnFlowStarted`, `OnStepChanged`, `OnStepThrew`, `OnAsyncSubmitted`, `OnAsyncCompleted`, `OnAsyncAbandoned`, `OnAsyncHighWater`, `OnFlowEnded`. 관심 있는 이벤트만 override 하면 된다. 훅 예외가 펌프를 죽이지 못한다.

---

## 챕터 8. 외부에서 구동하기, 그리고 생명주기

task 는 보통 펌프가 *아닌* 무언가가 런칭한다 - 이벤트 스레드, 소켓 콜백, main 스레드. `StartFlow()`(과 `StartTask`)는 어느 스레드에서나 안전하고 펌프를 즉시 깨우므로, 첫 step 이 다음 20ms 폴링이 아니라 즉시 돈다.

```python
# 어느 스레드에서나: task 런칭은 안전하고 펌프를 즉시 깨운다.
def on_message(msg):
    handler.current = msg
    handler.ctx.StartFlow()        # 런칭되면 Ok, 이미 돌고 있으면 Busy

# 자기 채널로 상태를 바꾼 뒤 펌프를 직접 깨울 수도 있습니다:
rt.Wake()
```

**오케스트레이터** 가 이 패턴을 라인 규모로 구동한다: 하나의 영속 task 가 매 라운드 모든 모듈의 `IsIdle()` 을 확인하고, 평범한 멤버 읽기로 그 모듈의 다음 task 를 런칭한다 - 워커 모듈은 스스로 시퀀싱하지 않는다. `pick_and_place.py` 의 형태다.

생명주기 제어:
- `module.IsIdle` - 비었나? 오케스트레이터가 다른 모듈의 다음 task 를 런칭하기 전에 확인. (`CurrentStepName()` / `CurrentStepOrdinal()` / `CurrentStepDescription()` 으로 도는 흐름의 위치 조회; idle 이면 `""` / `-1`.)
- `module.WaitUntilIdle(timeout=None)` - *이 모듈* 이 idle 될 때까지 호출 스레드 블록.
- `rt.WaitUntilIdle(timeout=None)` - *모든* 모듈이 idle 될 때까지 블록(`main` 이 종료 전 기다리는 법). 둘 다 step 안에서는 부르지 마세요.
- `module.Cancel()` - 도는 흐름을 협력적으로 종료(이유 `"cancelled"` 로 fail 표시); `rt.CancelAll()` 은 전체에.
- `rt.stop()` - 펌프 정지 + 풀 종료. `Runtime` 은 컨텍스트 매니저(`with uniflow.Runtime() as rt:`)이기도 해서 종료 시 자동 stop.

흔한 협력적 종료(모든 콘솔 예제가 쓰는)는 모듈 레벨 "stop" 플래그다 - 각 step 이 이를 확인해 `Done()` 을 반환하면 `WaitUntilIdle()` 이 반환되고 `rt.stop()` 이 전체를 정리한다.

---

## 다음

- [PYTHON_PORT.md](PYTHON_PORT.md) - 전체 API 표와 설계 결정.
- [python/examples](examples/) - 여섯 개의 예제([../cpp/examples](../cpp/examples/), [../cs/examples](../cs/examples/) 와 미러).
- [../cpp/TUTORIAL.md](../cpp/TUTORIAL.md) - C++ 튜토리얼; 같은 개념을 단일 펌프 모델·크로스-런타임까지 더 깊이.
- [../README.md](../README.md) - 프로젝트 개요.
