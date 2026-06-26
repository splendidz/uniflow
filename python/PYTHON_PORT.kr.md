# uniflow.py - Python 포팅 노트

> Language: **한국어** | [English](PYTHON_PORT.md)

> C++ `uniflow.hpp` 의 Python 형제(`uniflow.py`)에 대한 API & 설계 노트.
> **이 문서는 uniflow 라이브러리에 한정한다** - 특정 소비자(애플리케이션)에 대한 내용은 담지 않는다.

---

## 1. 무엇인가

`uniflow.py` 는 단일 스레드 협력적 step-driven 프레임워크인 `uniflow.hpp` 의 Python 포트다. 두 포트가 똑같이 읽히도록 **공개 표면이 C++ 이름을 그대로 미러링**하고(Task-Level Syntax), Python 에 불필요한 C++ 기교(CRTP/매크로/템플릿)는 옮기지 않는다.

핵심 개념(C++ 판과 동일):
- 하나의 관심사 = `Uniflow` 를 상속한 하나의 **모듈**.
- 모듈은 하나 이상의 **task**(`Task` 상속)를 소유하고 **한 번에 하나씩** 돌린다. task 는 단위동작이다: step 메서드 묶음 + 그들이 공유하는 상태.
- task 의 로직 = `StepResult` 의도를 반환하는 step 메서드들. 위에서 아래로 읽힌다. 첫 step 은 `Entry()` 가 지정하고, 다음 step 은 `self.Next(self.다음step)` 으로만 이동 -> **형태가 강제**되어 누가 짜도 동일. `Next` 는 현재 task 를 벗어나지 않는다.
- 하나의 `Runtime` 펌프 스레드가 attach 된 모든 모듈을 라운드로빈으로 한 tick 씩 구동하므로, 여러 모듈이 한 스레드에서 동시적으로 진행한다. 락 없음.
- 블로킹 작업은 `self.SubmitAsync(...)` 로 스레드풀에 offload 하고 `AsyncId` 를 즉시 받는다; 뒤 step 이 `self.AsyncResult(id)` 로 폴링한다. 펌프는 절대 막히지 않는다.

`uniflow.py` 는 **단일 파일, 표준 라이브러리만** 사용한다 (vendoring 용이).

> **단위.** Python 포트의 시간 단위는 **초** 다(C++ 포트는 밀리초). `rt.clock.Now()`, `UFTimer`, `StayTimeout` / `StayUntil` 마감, async 타임아웃에 모두 적용된다.

---

## 2. 공개 API

### 모듈과 task

| 구성요소 | 내용 |
|---|---|
| `Uniflow` | **모듈** 베이스. `__init__(rt, *, name=None)` (`name` 키워드 전용). async 슬롯, 내장 `self.uf_timer`, 도는 흐름의 위치를 소유 |
| ↳ task 바인딩 | `AddTask(task)` (`task.flow()` 를 이 모듈로 연결; 아무것도 시작 안 함), `StartTask(task) -> StartResult` (이 모듈을 `task.Entry()` 에서 런칭; 어느 스레드에서나 호출 가능) |
| ↳ 조회 | `IsIdle` (비었나?), `WaitUntilIdle(timeout=None)` (*이 모듈* 만 idle 까지 블록), `InstanceName()`, `CurrentStepName()` / `CurrentStepOrdinal()` / `CurrentStepDescription()` (실시간 "흐름이 지금 어디?"; idle 이면 `""` / `-1`), `Cancel()` (도는 흐름을 Fail 로 종료, 이유 `"cancelled"`), `Describe(*parts)` |
| `Task` | **task** 베이스(보통 모듈에 중첩). 상속해서 step 메서드를 정의하고, `Entry()`(첫 step 지정)와 선택적으로 `OnEnter()`(진입 시 per-task 상태 재무장)를 override |
| ↳ 모듈 접근 | `self.flow()` -> 소유 `Uniflow` (`AddTask` 가 연결), 예: `self.flow().some_attr` |
| ↳ step 의도 | `Stay()`, `StayTimeout(timeout_sec, timeout_step, *args, **kwargs)` (현재 step 을 계속 폴링하되 step 진입 후 논리 시간 `timeout_sec` 초과 시 `timeout_step` 으로 전이 = step 단위 catch; 성공 경로는 본문이 소유), `StayUntil(condition, settle_sec, success, timeout_sec, timeout_step)` (대기 조건을 접은 형태: `condition` 폴링해 `settle_sec` 유지되면 `success`, 아니면 타임아웃 시 `timeout_step`), `Next(fn, *args, **kwargs)` (형제 step 으로 진행, 인자 전달), `Done()`, `Fail(reason="")`, `Describe(*parts)` (현재 step 의 "지금 뭐 하는지" 한 줄; 다음 전이 때 찍히고 비워짐) |
| ↳ 런칭 | `StartFlow() -> StartResult` (`module.StartTask(self)` 의 슈가), `StartTask(other_task)` (흐름 도중 같은 모듈의 다른 task 로 전환; task 안에서 task 를 건너뛰는 유일한 방법) |
| ↳ 비동기 | `SubmitAsync(fn, label, timeout_sec=None, *args) -> AsyncId` (블로킹 작업 offload; `fn` 은 task 접근 없는 모듈 레벨/정적 함수; `timeout_sec` 실제 초, `None`=없음; in-flight 캡 초과 시 `0` 반환), `AsyncResult(id) -> AsyncOutcome`, `AnyAsyncPending() -> bool`, `ClearAsync()` (모든 진행 중 워커 폐기) |

### 의도, 비동기, 시계

| 구성요소 | 내용 |
|---|---|
| `StepAction` | `STAY` / `NEXT` / `DONE` / `FAIL` (의도; 상태 변경이 아님) |
| `StepResult` | step 반환값. `action`, `next_fn`, `next_name`, `reason`, `timeout_sec`(StayTimeout/StayUntil), `cond` / `settle_sec` / `success_fn` / `success_name`(StayUntil folded) |
| `StartResult` | `StartFlow` / `StartTask` 결과: `Ok`(런칭됨) / `Busy`(이미 task 가 이 모듈에서 도는 중) |
| `AsyncState` | `NotFound` / `Pending` / `Done` / `Failed` / `TimedOut` |
| `AsyncOutcome` | `AsyncResult(id)` 의 by-value 스냅샷. `state`, `return_value`(Done 일 때만 유효); 술어 `ok()` / `pending()` / `failed()` / `is_timeout()` / `found()` |
| `VirtualClock` | 논리 시계. `Now()`(초), `SetScale(s)` / `Scale()`(배속), `Freeze()` / `Resume()` / `Frozen()`(정지). 기본은 실제 monotonic 시계 1:1 추종. `UFTimer` / `StayTimeout` / `StayUntil` 에만 적용; async/IO 마감과 펌프 낮잠은 실제 시간 유지 |
| `UFTimer` | 폴링 타이머. `UFTimer(clock=None)`(기본 실제 wall 시계, `rt.clock` 주면 그 가상 시계 추종), `Restart()`, `HeldFor(cond, seconds) -> bool`(조건이 `seconds` 동안 **연속으로** 참이면 True; 한 번이라도 false면 리셋 후 False; settling/디바운스), `Passed(seconds) -> bool`(조건 없는 고정 대기 경과), `Elapsed()` |
| `Config` | Runtime 별 sleep 노브(초): `idle_sleep_sec`, `stay_sleep_sec`, `step_interval_sleep_sec`, `max_inflight_async` |

### Runtime 과 Observer

| 구성요소 | 내용 |
|---|---|
| `Runtime` | 펌프 스레드 + `ThreadPoolExecutor` + observer + 시계. `__init__(*, threads=4, observer=None, config=None)` (기본 observer 는 `ConsoleObserver`) |
| ↳ | `WaitUntilIdle(timeout=None)`(전체 모듈), `CancelAll()`, `Post(fn)`(펌프 스레드에서 콜백 실행), `Wake()`(자는 펌프 즉시 깨움; 외부 이벤트 스레드용), `SetPreRound(fn)`(매 라운드 시작 훅), `clock` -> `VirtualClock` (`rt.clock.SetScale(10)` / `.Freeze()` / `.Resume()`), `observer`, `config`, `stop(join=True, timeout=2.0)`. 컨텍스트 매니저이기도 함 (`with Runtime() as rt:` -> 종료 시 stop) |
| `Observer` / `ConsoleObserver` | 모든 이벤트의 단일 출구. 베이스 `Observer` 는 no-op(조용); `ConsoleObserver`(기본)는 이벤트당 한 줄을 예쁘게 찍음. 훅: `OnFlowStarted`, `OnStepChanged(obj, prev_step, next_step, description, step_ordinal, elapsed_ms, ticks)`(`Describe` 텍스트 전달), `OnStepThrew`, `OnAsyncSubmitted`, `OnAsyncCompleted(obj, job, wait_ms, had_error, timed_out)`, `OnAsyncAbandoned`, `OnAsyncHighWater`, `OnFlowEnded(obj, terminal_action, final_step_ordinal, wall_ms, reason)`. 훅 예외가 펌프를 죽이지 못함 |
| `__version__` / `VERSION` | `"1.0.0"` / `(1, 0, 0)` |

최소 사용:

```python
import uniflow

class Flow_Router(uniflow.Uniflow):
    def __init__(self, rt):
        super().__init__(rt, name="Flow_Router")
        self.ctx = self.Task_Route()
        self.AddTask(self.ctx)

    class Task_Route(uniflow.Task):
        def Entry(self):                 # 흐름 시작점: 첫 step 지정
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

## 3. 무엇이 무엇에 대응되나 (C++ -> Python)

Python 포트는 C++ 공개 API 를 미러링한다; 언어 배관만 다르다.

| C++ (`uniflow.hpp`) | Python (`uniflow.py`) | 비고 |
|---|---|---|
| `class Flow_X : uniflow::Uniflow` | `class Flow_X(uniflow.Uniflow)` | 모듈 베이스; CRTP `Uniflow<Derived>` -> 평범한 상속 |
| `struct Task_Y : uniflow::Task<Flow_X>` | `class Task_Y(uniflow.Task)` | task 베이스; `flow()` 로 모듈 접근 |
| `AddTask(&task)` / `StartTask(task)` | `AddTask(task)` / `StartTask(task)` | 한 번 바인딩, 어느 스레드에서나 런칭 |
| `task.StartFlow()` | `task.StartFlow()` | `module.StartTask(self)` 슈가 |
| `UF_NEXT(StepFn, args...)` | `self.Next(self.StepFn, *args)` | 매크로 -> 메서드; 다음 step 에 인자 전달 |
| `Stay()` / `Done()` / `Fail(reason)` | `Stay()` / `Done()` / `Fail(reason="")` | 동일 의도 |
| `StayUntil(ms, StepFn)` | `StayTimeout(seconds, StepFn)` | ms -> 초; 개명 (StayUntil 은 이제 folded 조건 형태) |
| `SubmitAsync(fn, "label", ms, args...)` | `SubmitAsync(fn, "label", seconds, *args)` | `AsyncId` 반환; `0` = 거부 |
| `AsyncResult(id)` -> outcome | `AsyncResult(id)` -> `AsyncOutcome` | `ok()` / `pending()` / `failed()` / `is_timeout()`; `.return_value` |
| `UFTimer`, `HeldFor` / `Passed` / `Elapsed` | 동일 이름 | 배속/정지는 `rt.clock` 에 바인딩 |
| `clock.SetScale` / `Freeze` / `Resume` | `rt.clock.SetScale` / `Freeze` / `Resume` | 논리 시간 제어 |
| `Runtime`, `WaitUntilIdle`, `Wake`, `Post` | 동일 이름 | 펌프 + executor + observer + 시계 |
| `Observer` / `ConsoleObserver` | 동일 이름 | 베이스는 조용, 기본은 `ConsoleObserver` |

`name` 생략 시 클래스명은 `type(self).__name__` 에서 가져온다. `name` 은 **키워드 전용**(`__init__(rt, *, name=None)`): `Flow_X(rt, x)` 처럼 두 번째 위치인자를 흘리는 흔한 오용이 `name` 에 조용히 바인딩되지 않고 `TypeError` 로 즉시 드러난다.

---

## 4. 설계 결정

- **방법론에 충실, C++ 기교는 버림.** CRTP(`Uniflow<Derived>`) -> 평범한 상속. 매크로(`UF_NEXT` / `UF_START_FLOW` / `UF_ASYNC` 등) -> 메서드. 클래스명 -> `type(self).__name__`.
- **모듈이 task 를 소유; 한 번에 하나의 task.** 로직은 `Task` 서브클래스(보통 중첩)에 산다 - 각 단위동작이 자기 step 메서드와 공유 상태를 소유하도록. `Next` 는 task 안에서 진행하고, `StartTask` 가 같은 모듈의 다른 task 로 건너뛰는 유일한 task-내 방법이다.
- **블로킹 작업 = executor offload, id 폴링.** `SubmitAsync` 가 워커를 `ThreadPoolExecutor` 에 넣고 `AsyncId` 를 반환; 펌프가 매 라운드 완료된 id 를 sweep 하고 뒤 step 이 `AsyncResult` 로 `AsyncOutcome` 을 읽는다. 끝난 워커가 펌프를 `Wake()` 하므로 다음 폴링이 `stay_sleep_sec` 만큼 기다리지 않고 캐치한다. GIL 주의: I/O 바운드(네트워크/gRPC)는 블로킹 중 GIL 을 풀어 실제 동시성을 얻지만, CPU 바운드 step 은 GIL 때문에 병렬화되지 않는다 - CPU 무거운 일도 offload 가 원칙.
- **폴링 주기는 Runtime 일괄 관리, step 별 gate 없음.** 펌프 라운드 간 대기는 인터럽트 가능한 `Condition` 이고, `StartTask` / async 완료 / 외부 `Wake()` 가 즉시 인터럽트한다 - 느슨한 폴링 주기(`stay_sleep_sec`) 때문에 흐름 런칭·async 완료 캐치가 늦어지지 않게.
- **`HeldFor` 는 "deadline 안에 충족?" 이 아니라 "`seconds` 동안 연속 충족?"** (settling). 마감 기반의 "조건 못 오면 빠지기" 는 `StayTimeout(timeout, target)` 이 담당한다 (try/catch 의 catch 처럼 정리 step 으로 라우팅). 둘은 직교: `HeldFor` 로 조건 안정화를 보고, `StayTimeout` 으로 안 오는 경우의 탈출구를 둔다.
- **`SetPreRound(fn)`**: 매 라운드 시작 직전 1회 호출되는 훅. "라운드당 1회 갱신/폴링" 같은 공통 전처리를 모듈마다 중복하지 않게 한다.
- **1차 제외(필요 시 확장):** 크로스-런타임 `PostAndWait` / `Link`, `RoundProfile` / 슬로우-라운드 트레이싱, `FlowStats`. 예제가 아직 쓰지 않으므로 단일 파일을 작게 유지.

---

## 5. 상태

- **코어를 C++ 설계에 재정렬 완료.** 모듈 + task(`Uniflow` / `Task`), step 의도(`Stay` / `StayTimeout` / `StayUntil` / `Next` / `Done` / `Fail`), `StartTask` / `StartFlow` / `AddTask`, id 기반 비동기(`SubmitAsync` / `AsyncResult` / `AsyncOutcome` / `AnyAsyncPending` / `ClearAsync`), `VirtualClock`(`SetScale` / `Freeze` / `Resume`), `UFTimer`, `Config`, `Runtime`(펌프 + executor + `Post` / `Wake` / `SetPreRound` / `WaitUntilIdle` / `CancelAll` / `stop`), `Observer` / `ConsoleObserver`.
- **예제 6개 포팅** 완료, [examples/](examples/) 에 동봉 - C++([../cpp/examples](../cpp/examples/))·C#([../cs/examples](../cs/examples/)) 세트와 미러: `simulator.py`(가상 시계 - 배속/정지), `shared_ostream.py`(락 없는 공유 sink), `message_dispatch.py`(라우팅 + async 폴링), `pick_and_place.py`(오케스트레이터 + 멀티-task 모듈 + async 폴링 ack), `queue_drain.py`, `city_traffic.py`.
- **Python 3.14 검증**: 튜토리얼 핵심 샘플(Task `Next` 체인 + `StartFlow` + `WaitUntilIdle`; 워커 결과를 돌려주는 `SubmitAsync` + `AsyncResult` 폴링; `StayUntil` 마감을 줄이는 `VirtualClock.SetScale`; 논리 시간을 멈추는 `Freeze` / `Resume`; `UFTimer.HeldFor` settling)이 모두 클린 실행.

---

## 6. 배치 / git

- 정규 위치: `python/uniflow.py` (C++ 형제는 `cpp/uniflow.hpp`). uniflow 모노레포에 함께 올라간다.
- 언어별 디렉터리(`cpp/`, `python/`, `cs/`)로 구성. 한 언어가 파일 1개를 넘으면 그 디렉터리 안에서 나눈다.
