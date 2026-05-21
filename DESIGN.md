# uniflow-cpp — 단일 스레드 협동형 비동기 프레임워크

> 범용 단일 스레드 협동형 스케줄링 프레임워크. 메인 스레드 1개를 안전하게 보호하면서, 블로킹 작업은 풀로 빼고 결과는 다음 step에서 받는 패턴을 *구조적으로* 박아둔 미니 프레임워크.

작성일: 2026-05-19. 작업자: Andy Cho.

---

## 1. 한 줄 요약

각 모듈은 *step 함수들의 체인*으로 자기 흐름을 표현하고, runtime이 모든 모듈을 round-robin으로 pump하면서 step 전환·시간·async 대기를 *자동으로* 로깅한다. 블로킹 작업은 `UF_ASYNC(static_fn, args...)` 한 줄로 풀에 던지고, 다음 step에서 `AsyncResult<T>()` 로 결과를 받는다. 매직넘버·등록 리스트·람다·싱글톤·`this` shared state — *모두 없다*.

---

## 2. 어떤 문제를 푸는가

많은 프로그램은 외부에서 들어온 요청/명령을 *단계별 흐름*으로 처리한다. 예를 들어 주문 하나를 처리하는 흐름:

```
주문 도착 → 검증 → 가격 계산 → 결제(블로킹 IO) →
          → 결제 확인 → 주문 확정 → 완료
```

이런 흐름의 공통된 특징:
- **순서가 중요** — 잘못된 순서로 진행하면 잘못된 결과
- **블로킹 작업이 흩어져 있음** — 파일 IO, 네트워크 통신, 외부 서비스 호출
- **조정 로직을 한 스레드가 잡으면 단순해짐** — 공유 상태에 락이 필요 없고, 데이터 race가 원천 차단된다
- **사고 났을 때 "어디까지 갔다가 어디서 죽었는지" 추적이 critical** — 평소 6단계 가던 흐름이 2단계에서 죽었다, 같은 패턴 인식

이 패턴을 *구조적으로* 강제하려고 만든 게 이 프레임워크. 요청 핸들러, 작업 파이프라인, 연결 수명주기, 워크플로 엔진 — 흐름이 단계로 쪼개지고 그 사이에 블로킹이 끼는 곳이면 어디든 맞는다.

---

## 3. 핵심 디자인 결정 (그리고 *왜* 그렇게 결정했는지)

> 이 섹션은 미래의 자신이 "이거 왜 이렇게 했지?" 묻기 전에 답해두는 게 목적. 거부한 대안과 그 이유까지 담는다.

### 3-1. step = 멤버 함수, 흐름 = 본문 안의 `NEXT(...)` 체인

**선택한 모양:**
```cpp
StepResult OnCheckout_Validate() {
    if (!ok_) return Fail();
    return UF_NEXT(OnCheckout_Price);   // 다음 step 지정
}
```

각 step은 다음에 누구한테 넘길지를 *자기 본문 안*에서 결정한다. 흐름 정보가 한 군데에 모이지 않고 *각 함수에 분산*되어 있다.

**왜 이게 좋은가:**
- step 추가/삭제/순서 변경이 *그 step만 건드리는* 한 줄 변경. 다른 step 코드 0줄 수정.
- 두 명이 동시에 다른 step에 작업해도 git merge 충돌 가능 면적이 거의 0.
- "다음에 어디로 갈지" 분기가 step 본문 안에서 자연스러움 (early-exit jump, retry loop 등).

**거부한 대안들:**

| 대안 | 왜 거부 |
|---|---|
| switch + case 100, 101, ... | 매직넘버 hard-code, step 추가 시 뒤 번호 다 밀림 (cascading edit), 머지 충돌 좋아함 |
| switch + `case OFFSET+N` (constexpr base) | base만 한 군데지만 `+1, +2` 손번호 여전, 끼워넣으면 또 cascade |
| nested switch (group, offset) | 매직넘버는 사라지지만 한 함수 안에 모든 로직 집중 → 같은 파일 동시 편집 머지 충돌 |
| 생성자에서 `AddStep(name, &fn)` 등록 리스트 | step 한 줄 추가가 *등록 리스트와 함수 본문* 2군데 수정. 등록 리스트가 conflict hotspot |
| 생성자에서 `Step(name, [this]{...})` 람다 등록 | 한 군데 모이긴 하지만 람다 *디버깅 매우 불편*, 팀에 미친숙. **유저가 명시적으로 거부.** |
| C++20 coroutine (`co_await`) | 진짜 linked-list 느낌이고 코드가 가장 깔끔하지만, boilerplate 100~150줄, 코루틴 frame 디버깅 학습 비용, 임의 jump 자유도 낮음 → 도입 비용 vs 이득 비율로 보류 |
| 한 step = 한 클래스 (체인) | trivial step에 클래스 폭증. *복잡한 step만* 골라서 이 패턴으로 빼는 하이브리드는 OK |

### 3-2. 반환값으로 흐름 표현: `Stay / Advance / Done / Fail`

```cpp
StepResult OnWaitReady() {
    if (!ready) return Stay();           // 같은 step 다음 tick에 다시
    if (timeout) return Fail();          // 흐름 abort
    return UF_NEXT(OnNextStep);          // 진행
}
```

framework가 `_curr_step++` 같은 *상태 조작*을 가져가서, 유저는 *의도만* 반환. `Stay`/`Advance`/`Done`/`Fail` 4가지로 표현 가능한 모든 흐름 결정이 끝.

### 3-3. 싱글톤 없음 — `UniflowRuntime`이 객체로 모든 걸 소유

```cpp
UniflowRuntime rt(cfg);
rt.RegisterExecutor("default", std::make_shared<StdThreadPool>(4));
auto* order = rt.Create<OrderModule>("order");
rt.RunInBackground();
```

**왜:**
- 테스트 격리 (시뮬레이터 안에 여러 인스턴스 동시 띄우기)
- 도메인 분리 (라인 A / 라인 B 별도 runtime)
- 의존성이 코드에 *명시적으로* 보임 — 어디서 어떤 풀/observer를 쓰는지 grep 가능
- 싱글톤은 한 번 박히면 빼내기 비용이 막대. 처음부터 피한다.

### 3-4. Step number를 코드에서는 *제거*, 로그에는 *보존*

코드:
```cpp
StepResult OnCheckout_Validate() { ... }    // 번호 없음
```

로그:
```
[order]   #01 OnCheckout_Begin      cpu=0.02ms
[order]   #02 OnCheckout_Validate   cpu=0.00ms
...
[order] Checkout FAILED  reached #02/#06  ...
```

**왜:**
- 코드에 번호 있으면 cascading edit 문제 부활
- 사람은 번호에 직감이 강함 — "평소 6까지 가던게 2에서 죽었네" 패턴 인식
- runtime이 ordinal을 자동 부여 + 흐름별 평균 길이 (`max_seen_length`) 학습해서 `reached #N/#M` 형식으로 출력 → *현재/평균* 한 줄에 다 보임

### 3-5. 구조적 자동 로깅 — 수동 로그 심지 않게

framework가 자동으로 잡는 것:
- step 진입/종료
- step별 CPU 시간 (메인 스레드 점유)
- step 임계치 초과 시 `[WARN] SLOW CPU step` (싱글스레드 보호의 핵심 경보)
- async submit / completion / wait 시간
- async timeout / slow async 경보
- 흐름 trace 전체 (시간/순서) — DONE/FAIL 시 한 덩어리로 logger에 전달

유저 코드에 `LOG()` 한 줄 안 박아도 사고 분석 가능. **수동 로그는 항상 빠진 자리가 사고난 자리.**

### 3-6. `this` 접근 금지 — `static` 강제, 컴파일 타임

```cpp
UF_ASYNC(DoChargeCard, total_);
//        ^^^^^^^^^^^^
//        static 멤버 함수만 허용 (static_assert)
```

```cpp
static ChargeResult DoChargeCard(double amount) {
    // this 없음. 멤버 변수 일체 접근 불가.
    // 필요한 모든 데이터는 args로 받음.
}
```

**왜 static 강제:**
- worker 스레드에서 `this` 만지면 메인 스레드와 race
- "조심하라"는 컨벤션은 사람이 어긴다. 컴파일러로 강제하면 *어길 수 없다*.
- `static_assert(!std::is_member_function_pointer_v<decltype(Fn)>)` 한 줄로 시점 빌드 단계로 당김

**추가 안전망**: `SubmitAsync`가 args를 `std::tuple<std::decay_t<Args>...>` 로 복사/이동. 참조 우회도 차단.

### 3-7. Async 결과는 typed slot, 멤버 변수 0개

```cpp
StepResult OnCheckout_ChargeDone() {
    auto r = AsyncResult<ChargeResult>();   // base가 보관, 유저 멤버 깨끗
    if (r.is_timeout()) return Fail();
    if (r.failed())     return Fail();
    charged_ = r.value().amount;
    return UF_NEXT(OnCheckout_Confirm);
}
```

`std::future`, `wait_for`, `get`, `try/catch` 모두 유저 코드 0줄. base가 typed slot에 보관, continuation step에서 `AsyncResult<T>()` 로 꺼냄.

### 3-8. Async 디테일은 *호출처에서* 결정

```cpp
AsyncOpts opts;
opts.timeout         = 3s;
opts.slow_warn_after = 200ms;
opts.executor_name   = "payments";       // named pool 선택
UF_ASYNC_OPT(DoChargeCard, opts, total_);
```

타임아웃/풀선택/조기경보 임계가 *그 async 호출 옆*에 박힘. 흩어지지 않음.

### 3-9. CPU 시간 vs Async 대기 시간 *분리* 측정

```
#04 OnCheckout_Charge         cpu=0.01ms          ← 메인 스레드 점유 (submit 비용만)
    +- async DoChargeCard     wait=228.27ms       ← 풀에서 실제 작업 시간
```

같은 step의 "CPU 시간"으로 합치면 운영자가 헛다리 짚음 (싱글스레드 보호 경보의 정확성). **반드시 분리.**

---

## 4. 어휘 / 명명 규약

| 용어 | 의미 |
|---|---|
| **step** | 실행 단위. 멤버 함수 1개 = step 1개 |
| **flow** | 이름붙은 step 체인 (예: "Checkout"). 외부에서 `Start("flow_name")`로 진입 |
| **runtime** | 객체 소유 + pump 루프 + executor/observer 보관 |
| **ordinal** | flow 시작 이후 몇 번째 step 전환인가 (로깅용 카운터) |
| **executor** | 스레드풀 추상화 (named, 여러 개 등록 가능) |
| **observer** | 모든 step 이벤트의 sink (로그/메트릭/테스트 hook) |

비동기로 푸는 작업은 코드 전반에서 *async function* 또는 *task*로 부른다 (도메인 명사와 섞이지 않게 일관 유지).

### 클래스/타입
- `uniflow::Uniflow<Derived>` — CRTP base
- `uniflow::UniflowRuntime`
- `uniflow::StepResult` + `uniflow::StepAction` (enum: Stay/Advance/Done/Fail)
- `uniflow::AsyncOpts`, `uniflow::AsyncRef<T>`
- `uniflow::IExecutor`, `uniflow::StdThreadPool`, `uniflow::InlineExecutor`, `uniflow::BSThreadPoolExecutor`
- `uniflow::IUniflowObserver`, `uniflow::ConsoleObserver`
- `uniflow::TraceEntry`, `uniflow::FlowStats`

### 매크로 (모두 `UF_` 접두사)
- `UF_USES_UNIFLOW(MyClass)` — 클래스 본문에 한 번. base에 friend 권한 부여 (private static 접근용)
- `UF_ENTRY(flow_name, fn)` — 생성자에서 flow 등록
- `UF_NEXT(fn)` — step 함수 안에서 다음 step 지정
- `UF_ASYNC(fn, args...)` — 풀에 static fn 던지기 (default opts)
- `UF_ASYNC_OPT(fn, opts, args...)` — opts 명시

### 코드 컨벤션
- derived 첫 줄에 `using S = ThisClass;` (매크로가 `&S::xxx` 로 접근)
- step 함수 이름: `On<FlowName>_<Purpose>` (예: `OnCheckout_Validate`)
- async 결과 받는 step: `On<FlowName>_<TaskName>Done` (예: `OnCheckout_ChargeDone`)
- 멤버 변수: `trailing_underscore_`

---

## 5. 유저 코드 모양 (요약)

```cpp
class OrderModule : public uniflow::Uniflow<OrderModule> {
    using S = OrderModule;
    UF_USES_UNIFLOW(OrderModule);            // base가 private static 접근 가능하게

public:
    OrderModule(uniflow::UniflowRuntime& rt) : Uniflow(rt) {
        UF_ENTRY("Checkout", OnCheckout_Begin);   // 외부 진입점 등록 (flow 단위)
    }

    bool RequestCheckout(int items, double unit_price, double discount) {
        if (!IsIdle()) return false;
        items_ = items; unit_price_ = unit_price; discount_ = discount;
        return Start("Checkout");
    }

private:
    StepResult OnCheckout_Begin()    { return UF_NEXT(OnCheckout_Validate); }
    StepResult OnCheckout_Validate() {
        if (items_ <= 0) return Fail();
        return UF_NEXT(OnCheckout_Charge);
    }
    StepResult OnCheckout_Charge() {
        uniflow::AsyncOpts opts;
        opts.timeout = 3s; opts.slow_warn_after = 200ms;
        opts.executor_name = "payments";
        total_ = items_ * unit_price_ - discount_;
        UF_ASYNC_OPT(DoChargeCard, opts, total_);
        return UF_NEXT(OnCheckout_ChargeDone);
    }
    StepResult OnCheckout_ChargeDone() {
        auto r = AsyncResult<ChargeResult>();
        if (r.is_timeout() || r.failed() || !r.value().ok) return Fail();
        charged_ = r.value().amount;
        return Done();
    }

    struct ChargeResult { bool ok; double amount; double took_ms; };
    static ChargeResult DoChargeCard(double amount);

    int    items_ = 0;
    double unit_price_ = 0, discount_ = 0, total_ = 0, charged_ = 0;
};
```

---

## 6. 현재 파일 구성

| 파일 | 역할 |
|---|---|
| [include/uniflow.hpp](include/uniflow.hpp) | 프레임워크 전체 (header-only, ~580줄). CRTP base, Runtime, Result, executors, observers, 매크로 |
| [include/BS_thread_pool.hpp](include/BS_thread_pool.hpp) | 벤더링한 BS::thread_pool v4.1.0. 선택 — `UNIFLOW_USE_BS_THREAD_POOL` 정의 시에만 사용 |
| [examples/demo/order_module.h](examples/demo/order_module.h) / [.cpp](examples/demo/order_module.cpp) | 주문 체크아웃 샘플. 단일 flow `Checkout` |
| [examples/demo/session_module.h](examples/demo/session_module.h) / [.cpp](examples/demo/session_module.cpp) | 연결 세션 샘플. 두 flow `Open`, `Close` 한 모듈에 |
| [examples/demo/main.cpp](examples/demo/main.cpp) | 6개 시나리오 데모 (성공/jump/fail/concurrent) |
| [examples/tutorials/](examples/tutorials/) | 한 파일짜리 예제 6개 — `01`~`03` 프레임워크 기본(step 체인 / async / `Stay()`), `04`~`06` 고전 동시성 문제(생산자-소비자 / 식사하는 철학자 / 계좌 이체)를 락 없이 |
| [vs2022/uniflow-cpp.sln](vs2022/uniflow-cpp.sln) | Visual Studio 2022 솔루션 — 데모 + 튜토리얼 프로젝트 |
| [notes/make_concept.hpp](notes/make_concept.hpp) | 처음 브레인스토밍 파일. *역사적 기록*으로 남김. 현재 코드와 일치하지 않음 |
| [DESIGN.md](DESIGN.md) | 이 파일 |

### 모듈을 .h+.cpp로 나눈 이유
프레임워크는 어차피 거의 template이라 header-only가 자연스럽지만, 유저 모듈은:
- 모듈 늘어날 때 .h만 include하면 cpp 본문 안 끌려옴 → 빌드 시간 통제
- cpp에 헬퍼/도메인 자료를 anonymous namespace로 숨길 수 있음
- 다른 모듈에서 forward declaration 가능

작은 데모 한 두개라면 한 .hpp로 합쳐도 OK. 규모 커지면 분리 권장.

---

## 7. 빌드 / 실행

모든 명령은 저장소 루트에서 실행한다. 산출물은 `build/`로 들어간다.

표준은 **C++17**이다 (필요한 건 `std::any` / `if constexpr` / `template<auto>` / `std::optional` / `std::string_view` — 모두 C++17).

### Linux / WSL
```bash
g++ -std=c++17 -O2 -pthread -I include \
    examples/demo/main.cpp examples/demo/order_module.cpp examples/demo/session_module.cpp -o build/demo
./build/demo
```

### BS::thread_pool 같이 쓸 때
`BS_thread_pool.hpp`는 이미 [include/](include/)에 벤더링돼 있다 (BS::thread_pool v4.1.0). `-DUNIFLOW_USE_BS_THREAD_POOL`만 추가하면 된다:
```bash
g++ -std=c++17 -O2 -DUNIFLOW_USE_BS_THREAD_POOL -pthread -I include \
    examples/demo/main.cpp examples/demo/order_module.cpp examples/demo/session_module.cpp -o build/demo
```

### Windows / MSVC
```cmd
cl /std:c++17 /EHsc /O2 /I include ^
   examples\demo\main.cpp examples\demo\order_module.cpp examples\demo\session_module.cpp /Fe:build\demo.exe
```
`UF_ASYNC` 매크로는 `, ##__VA_ARGS__`(GNU 콤마 생략 형태)를 쓴다 — GCC·Clang·MSVC 프리프로세서 모두 지원하므로 `/Zc:preprocessor` 같은 특별한 플래그가 필요 없다. BS::thread_pool 옵션은 `/DUNIFLOW_USE_BS_THREAD_POOL`. `pthread` 옵션은 필요 없음.

### Visual Studio 2022
[vs2022/uniflow-cpp.sln](vs2022/uniflow-cpp.sln)을 열고 프로젝트(`demo` / `01_minimal` / `02_async` / `03_polling`)를 골라 x64로 빌드. 컴파일러 옵션(C++17 등)은 `.vcxproj`에 이미 설정돼 있다.

### 데모 동작 (요지)
```
=== scenario 1 ===  [order]   Checkout DONE   reached #06/#06  async=62.76ms
=== scenario 2 ===  [order]   Checkout DONE   reached #04/#06  async=0.00ms   (early-exit jump)
=== scenario 3 ===  [order]   Checkout FAILED reached #02/#06  async=0.00ms   (empty cart)
=== scenario 4 ===  [session] Open  DONE   reached #07/#07  async=283.99ms
                    [session] Close DONE   reached #05/#07  async=103.72ms
=== scenario 5 ===  [session] Close FAILED reached #01/#07  async=0.00ms   (nothing open)
=== scenario 6 ===  order + session 진짜 병렬 — 로그가 interleave
```

`cpu` 합이 항상 100μs 미만 = 메인 스레드 거의 0 점유. 의도대로.

---

## 8. 알려진 디테일 / 다음에 두드릴 것

### 이미 합의·구현된 것
- per-object `std::mutex`로 외부 `Start()` 와 pump의 `ExecuteOnce()` race 차단
- ConsoleObserver는 ASCII 글리프 (Windows 콘솔 호환)
- `Config cfg = {}` 디폴트 인자 대신 overload 분리 (g++ 11 호환)
- `UF_USES_UNIFLOW` 매크로로 base friend 부여 (g++ NTTP 접근 체크 회피)
- StdThreadPool 번들 (InlineExecutor는 *테스트 전용* — 데모에서 쓰면 메인 스레드 점유 헛다리 경보)

### 미해결 / 향후 결정 필요
- [ ] **흐름 trace의 외부 export** — 현재 ConsoleObserver는 trace 사용 안 함. 사고분석용 파일/JSON dump observer 필요할 수 있음
- [ ] **이벤트 ring buffer** — 실시간 UI에서 최근 N개 trace 보여주려면 별도 observer
- [ ] **histogram 통계** — step 이름별 p50/p99 누적 (운영중 anomaly 자동 감지). FlowStats 확장 자리
- [ ] **에러 코드 표준화** — 지금 `Fail()`은 흐름만 종료. 어떤 에러였는지 호출자에게 돌려줘야 할 수도 (예: 외부 API에서 흐름 trigger 후 결과 받기)
- [ ] **외부 결과 받기 (promise back)** — `Start("flow")` 가 future를 반환해서 호출자가 결과를 기다리거나, callback 등록할 수 있게
- [ ] **Cancellation** — 진행 중 흐름을 외부에서 abort 시키기. std::future는 캔슬 모름 → 협력형 캔슬 (`shared_ptr<atomic<bool>>`) 도입
- [ ] **명령 mailbox 패턴** — 외부 명령을 메인 스레드 외부에서 안전하게 큐잉. 지금은 mutex로 막아두긴 했지만, mailbox 쪽이 production-grade
- [ ] **다수 in-flight async** — 현재 한 객체당 async 1슬롯. 병렬 IO 필요해지면 named slot (`UF_ASYNC("copy", ...)` + `AsyncResult<T>("copy")`)
- [ ] **에러 메시지 풍부화** — Fail 시 reason string 캡쳐
- [ ] **테스트 코드** — InlineExecutor + NullObserver 조합으로 deterministic 흐름 검증. 단위 테스트 framework 결정 필요 (catch2? doctest? gtest?)

### 실제 도메인 코드로 넘어갈 때
프레임워크는 더 손댈 게 거의 없음. 실제 작업은 **도메인 모듈 만들기**:
- 외부 명령 수신 → 적절한 모듈로 라우팅하는 디스패처 모듈
- 요청 종류별 처리 흐름 모듈 (한 종류 = 한 모듈)
- 외부 시스템(파일 / 네트워크 / DB) 추상화 모듈
- Queue 모듈 (모듈 점유 중일 때 backlog)

각 모듈마다 [examples/demo/order_module.cpp](examples/demo/order_module.cpp) 같은 패턴 그대로 적용.

---

## 9. 회의·결정 이력 (요약)

이 디자인이 한 번에 나온 게 아니라, 여러 차례 안을 던지고 거부하면서 도달함. 거부의 *이유*가 디자인의 본질.

1. **switch + 매직넘버** → 매직넘버 cascade, merge 충돌
2. **Bind 테이블 (등록 + 함수)** → 두 군데 동기 수정, conflict
3. **constexpr offset (`OFFSET + 1`)** → 손번호 cascade 여전
4. **enum offset + nested switch** → 같은 파일 동시 편집 conflict
5. **람다 등록형** → 람다 디버깅 불편, 팀 미친숙 → **유저 명시 거부**
6. **멤버 함수 + 등록 리스트** → 등록 리스트가 conflict hotspot
7. **체인 = 본문 안 `NEXT(...)`** → **채택**. 위치 의존성 0, 머지 충돌 면적 최소
8. async 패턴: future 멤버 → 거부 (오염), continuation 인자 → 거부 (시그니처 통일 못 함), 출력 인자 → 거부 (예외 표현 안 됨), **typed slot + `AsyncResult<T>()`** → 채택
9. async 함수 안전: `static` + decay tuple → 컴파일 강제로 채택
10. 로그 attribution: cpu vs async 분리 → 채택 (헛다리 방지)
11. step number: 코드 제거 / 로그 보존 + ordinal + 평균 길이 (`#N/#M`) → 채택
12. 풀: 추상화 (`IExecutor`) + 번들 (`StdThreadPool`, `InlineExecutor`) + 어댑터 (`BSThreadPoolExecutor`) → 채택
13. 싱글톤 거부 → `UniflowRuntime` 객체 + 주입

이 결정들의 *반대 방향* 으로 가려면 위 이유들을 먼저 무력화하는 새 사실이 있어야 함.

---

## 10. 다음 세션 진입 가이드

이 doc 읽고 → [include/uniflow.hpp](include/uniflow.hpp) 헤더 주석 훑고 → [examples/demo/order_module.cpp](examples/demo/order_module.cpp) 의 한 flow 따라가보면 30분 안에 컨텍스트 복원 가능.

진짜 다음 작업으로 갈 때 추천 순서:
1. **테스트 인프라** — InlineExecutor + NullObserver로 한 모듈에 대한 deterministic 테스트 한 개 작성. 패턴 굳히고 다른 모듈에 복제.
2. **에러 reason** — `Fail(reason_code)` 또는 `Fail(message)` 형태로 흐름 종료 시 정보 보존.
3. **외부 결과 받기** — `Start` 가 future를 반환하거나 callback 등록 가능하게. 외부 명령 처리 흐름에서 필수.
4. **첫 실제 도메인 모듈** — MessageHandler 같이 진짜 명령 라우팅 모듈 하나 만들면서 framework 부족한 부분 드러내기.

순서 바꿔도 됨. 막힐 때마다 *왜 막히는지*를 위 결정 이력과 대조해서 디자인이 안 맞는 건지 그냥 구현 디테일인지 구분.
