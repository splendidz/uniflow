# uniflow.py — Python 포팅 진행 노트

> 🌐 언어: **한국어** | [English](PYTHON_PORT.md)

> C++ `uniflow.hpp` 의 Python 형제(`uniflow.py`)에 대한 설계/진행 기록.
> **이 문서는 uniflow 라이브러리에 한정한다** — 특정 소비자(애플리케이션)에 대한 내용은 담지 않는다.

---

## 1. 무엇인가

`uniflow.py` 는 단일 스레드 협력적 step-driven 프레임워크인 `uniflow.hpp` 의 Python 구현이다.
**멘탈모델/동작에 충실**하되, Python 에서 불필요한 C++ 기교(CRTP/매크로/템플릿)는 옮기지 않는다.

핵심 개념(C++ 판과 동일):
- 하나의 관심사 = `Uniflow` 를 상속한 하나의 클래스.
- 그 클래스의 로직 = `StepResult` 를 반환하는 멤버 함수들의 체인. 위에서 아래로 읽힌다.
- 다음 step 은 `self.Next(self.다음멤버)` 로만 이동 → **형태가 강제**되어 누가 짜도 동일(정형화).
- 하나의 `Runtime` 펌프 스레드가 attach 된 모든 모듈을 라운드로빈으로 한 tick 씩 구동 →
  여러 모듈이 한 스레드에서 **마치 병렬처럼** 진행. 락 없음.
- 블로킹 작업은 `self.run_async(fn, then=...)` 로 스레드풀에 offload 하고, 완료까지 Stay 로
  펌프에 제어권을 돌려준다.

`uniflow.py` 는 **단일 파일, 표준 라이브러리만** 사용한다 (vendoring 용이).

---

## 2. 공개 API

| 구성요소 | 내용 |
|---|---|
| `StepAction` | `STAY` / `NEXT` / `DONE` / `FAIL` (의도; 상태 변경이 아님) |
| `StepResult` | step 반환값. `action`, `next_fn`, `next_name`, `reason`, `timeout_sec`(StayUntil 전용) |
| `VirtualClock` | 논리 시계. `now()`, `set_scale(s)`(배속), `freeze()`/`resume()`(정지), `scale`/`frozen`. 기본은 실제 시계 1:1 추종. UFTimer/StayUntil 에만 적용, async/IO·펌프 낮잠은 실제 시간 유지 |
| `UFTimer` | 폴링 타이머. `UFTimer(clock=None)`(기본 실제 시계, `rt.clock` 주면 그 가상 시계 추종), `init()`(=`restart`), `held_for(cond, duration)→bool`(조건이 duration 동안 **연속으로** 참이면 True, 중간에 거짓이면 누적 리셋 후 False; settling/debounce 판정), `passed(duration)→bool`(조건없는 고정 대기 경과 여부), `elapsed()` |
| `Uniflow` | 상속 베이스. `__init__(rt, *, name=None)` |
| ↳ step 헬퍼 | `Stay()`, `StayUntil(timeout_sec, on_timeout)`(현재 step 을 계속 폴링하되 step 진입 후 timeout_sec 초과 시 on_timeout step 으로 자동 전이 = step 단위 catch), `Next(fn)`, `Done()`, `Fail(reason="")` |
| ↳ 타이머 | `self.uf_timer` (내장 `UFTimer`) — **start/매 Next 전이 시 자동 init**. 대기 step 에서 `held_for(cond, duration)` 로 조건이 그 step 안에서 duration 동안 안정적으로 유지됐는지 판정 (수동 `init()` 불필요). step 경계 누적 측정은 별도 `uf.UFTimer()` |
| ↳ 비동기 | `run_async(fn, then)` — 블로킹 작업 offload, 완료 시 결과는 `self.async_value`. 잡이 끝나면 펌프를 자동 `wake()` 해서 다음 step 의 완료 캐치가 폴링 주기만큼 늦지 않게 함 |
| ↳ 제어 | `start(first_step)`, `cancel()`, `is_idle()`, `.name` |
| `Runtime` | 펌프 스레드 + `ThreadPoolExecutor` + Observer |
| ↳ | `wait_until_idle(timeout)`, `cancel_all()`, `any_failed()`, `submit(fn)`, `set_pre_round(fn)`, `wake()`(자고 있는 펌프를 즉시 깨움; 외부 이벤트 스레드용), `clock`(이 런타임 논리 시계 = `VirtualClock`; `rt.clock.set_scale(10)`/`.freeze()`), `stop()` |
| `Observer` / `ConsoleObserver` | 모든 이벤트의 단일 출구 (flow/step/async/threw/ended) |

최소 사용:

```python
import uniflow as uf

class OrderRouter(uf.Uniflow):
    def on_begin(self):              # 흐름 시작점
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

## 3. 설계 결정

- **방법론에 충실, C++ 기교는 버림.** CRTP(`Uniflow<Derived>`) → 평범한 상속. 매크로
  (`UF_NEXT`/`UF_START_FLOW`/`UF_ASYNC` 등) → 메서드/일반 함수. 클래스명 → `type(self).__name__`.
- **`name` 키워드 전용** (`__init__(rt, *, name=None)`): `Foo(rt, x)` 처럼 두 번째 위치인자를
  넘기는 흔한 오용이 `name` 에 조용히 바인딩되지 않고 `TypeError` 로 즉시 드러나게 한다.
- **블로킹 작업 = executor offload.** `run_async` 가 future 를 폴링하며 완료까지 Stay.
  GIL 주의: I/O 바운드(예: 네트워크/gRPC)는 블로킹 중 GIL 을 풀어 실제 동시성을 얻지만,
  CPU 바운드 step 은 GIL 때문에 병렬화되지 않는다 — CPU 무거운 일도 offload 가 원칙.
- **`set_pre_round(fn)`**: 매 라운드 시작 직전 1회 호출되는 훅. "라운드당 1회 갱신/폴링" 같은
  공통 전처리를 모듈마다 중복하지 않게 한다.
- **폴링 주기는 Runtime 일괄 관리, step 별 gate 없음.** 이전의 `Stay(gate_sec)` 는 제거했다
  (모든 step 이 자기 깨우는 시각을 정하는 건 부담). 대신 펌프 라운드 간 대기를 인터럽트 가능한
  Condition 으로 두고, `start()`/`run_async` 완료/외부 이벤트가 `wake()` 로 즉시 깨운다 —
  느슨한 폴링 주기(stay_sleep) 때문에 흐름 시작·async 완료 캐치가 늦어지지 않게.
- **`held_for` 는 "deadline 안에 충족?" 이 아니라 "duration 동안 연속 충족?"** (settling).
  마감 기반의 "조건 못 오면 빠지기" 는 `StayUntil(timeout, target)` 이 담당한다 (try/catch 의
  catch 처럼 정리 step 으로 라우팅). 둘은 직교: `held_for` 로 조건 안정화를 보고, `StayUntil`
  로 안 오는 경우의 탈출구를 둔다.
- **1차 제외(필요 시 확장):** 크로스-런타임 `Post`/`PostAndWait`, `Link`, `RoundProfile`/
  슬로우-라운드 트레이싱, `FlowStats`. TC/예제가 아직 쓰지 않으므로 단일 파일을 작게 유지.

---

## 4. 상태

- ✅ **구현 완료** — `StepAction`/`StepResult`, `Uniflow`(Stay/Next/Done/Fail, run_async,
  start/cancel/is_idle), `Runtime`(펌프 + executor + set_pre_round + wait_until_idle/
  cancel_all/any_failed/stop), `Observer`/`ConsoleObserver`.
- ✅ **스모크 검증** (Python 3.14): ① 멤버함수 체인(Stay→Next→Done) 완주, ② 두 모듈이 한
  펌프에서 `A,B,A,B` 인터리브(=병렬처럼), ③ `run_async` offload(펌프 안 막힘), ④ step 예외
  → Fail 격리 + `fail_exc` 보존 + `any_failed` 집계.

---

## 5. 배치 / git

- 정규 위치: `python/uniflow.py` (C++ 형제는 `cpp/uniflow.hpp`). uniflow 모노레포에 함께 올라간다.
- 언어별 디렉터리(`cpp/`, `python/`, 추후 `csharp/`)로 재구성 완료. 한 언어가 파일 1개를
  넘으면 그 디렉터리 안에서 나눈다.
