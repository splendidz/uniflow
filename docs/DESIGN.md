# uniflow — 단일 스레드 협동형 비동기 프레임워크

> 범용 단일 스레드 협동형 스케줄링 프레임워크. 메인 스레드 1개를 안전하게 보호하면서, 블로킹 작업은 풀로 빼고 결과는 다음 step에서 받는 패턴을 *구조적으로* 박아둔 미니 프레임워크.

최초 작성 2026-05-19 · **컨셉 전면 개정 2026-05-21** · 헤더·예제 재구현 2026-05-22 · 작업자: Andy Cho.

> **문서 상태** — 이 설계는 [cpp/uniflow.hpp](../cpp/uniflow.hpp)에 구현 완료됐고, [cpp/examples/](../cpp/examples/)의 두 예제로 검증됐다. 구현하며 추가로 확정된 사항은 §9에 정리. 구버전 튜토리얼·데모는 [temp/](temp/)에 보관(참조하지 않음).

---

## 1. 한 줄 요약

각 모듈은 *step 함수들의 체인*으로 자기 흐름을 표현한다. 런타임은 모든 모듈을 round-robin으로 pump하면서 step 전환·CPU 시간·async 대기를 *자동으로* 로깅한다. 블로킹 작업은 `UF_ASYNC(static_fn, args...)` 한 줄로 풀에 던지고, 다음 step에서 `AsyncResult<T>()`로 결과를 받는다. **런타임 객체는 유저에게 보이지 않고**, flow는 **함수 포인터로 진입**하며, 모듈은 **타입 단위로 식별**된다. 매직넘버·매직스트링·등록 리스트·람다·`this` shared state — 모두 없다.

---

## 2. 이번 개정에서 무엇이 바뀌었나

구버전(2026-05-19) 대비 인터페이스 컨셉이 바뀐 부분. 실행 모델(step 체인, async, 로깅)은 그대로다.

| 영역 | 구버전 | 개정 |
|---|---|---|
| 런타임 | 유저가 `UniflowRuntime rt(cfg)` 객체를 들고 `rt.Create<T>()` | **숨김.** 정적 매니저가 소유. 기본 1개 lazy, `Init(n)`은 옵트인 |
| flow 진입 | `Start("Checkout")` — 매직스트링 + `UF_ENTRY` 등록 | `Start(&S::OnCheckout_Begin, args...)` — 함수 포인터 + 인자 전달 |
| flow 등록 | 생성자에서 `UF_ENTRY`, 런타임에 `flows_` 레지스트리 | **없음.** flow = 진입 함수에서 시작하는 step 체인 그 자체 |
| step 가시성 | 전부 private, `Request*()` public 래퍼로 감쌈 | 진입 step은 public, 중간 step은 private |
| 모듈 정체성 | `rt.Create<T>("instance_name")` — 항상 이름 필요 | 익명 1개 자동 허용(클래스명), 2개째부터 이름 강제 |
| 모듈 접근 | 포인터를 들고 다니거나 런타임에 질의 | `Cls::inst()` / `Cls::GetInst("name")` |

제거된 것: `UF_ENTRY` 매크로, `DefineFlow`, flow 이름 레지스트리, 유저가 보유하는 `UniflowRuntime` 객체, `Start(const char*)`.

---

## 3. 어떤 문제를 푸는가

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

**전형적인 실제 활용처.** 여러 host가 통신으로 메시지를 던지고, 받은 메시지를 종류별 처리 큐에 쌓고, 그 큐를 소비하는 스레드들이 공유 자원에 접근한다 — 흔하디흔한 레퍼토리고, 이렇게 꼬이면 폰노이만이 와도 풀기 어렵게 꼬인다. *그 상황 자체를 만들지 않으려고* 이 프레임워크를 쓴다. 메시지 처리 로직을 step 체인으로 표현하면, 모든 step이 한 스레드 위에서 협동 실행되므로 공유 자원 접근이 암묵적 임계 구역이 된다.

이 프레임워크를 쓰는 사람이 멀티스레드 동기화의 고수일 거라고 가정하지 않는다. 오히려 그 반대다 — 동기화·공유 자원 문제를 자기 코딩 스킬로 커버할 수 있는 사람은 애초에 이런 방법론을 찾지 않는다. 그래서 API는 *흔한 경우를 의례 없이* 만들 수 있어야 하고, 위험한 경우(멀티 런타임 등)만 명시적 선택을 요구해야 한다.

---

## 4. 실행 모델

모듈의 로직은 **flow** — step 함수들의 체인 — 하나다. 런타임이 그 step들을 직접 루프 돌며 부르지 않는다. 세 계층이 안쪽으로 호출해 들어간다:

```
   pump 루프                   스레드 하나 (런타임당 1개)
        │   매 라운드, flow가 도는 모든 모듈에 대해:
        ▼
   module.ExecuteOnce()        그 모듈을 정확히 step 하나만큼 전진
        │   모듈의 현재 step 함수를 호출
        ▼
   StepResult OnSomeStep()     당신 코드 — 실행된 뒤 의도(intent)를 반환:

        ├─ UF_NEXT(OnNextStep)   advance — 런타임이 커서를 옮김
        ├─ Stay()                hold    — 커서 그대로, 다음 라운드 재실행
        ├─ Sleep(d)              hold    — 커서 그대로, d 경과 후 재실행
        └─ Done() / Fail()       end     — flow 종료, 모듈은 idle
```

런타임은 **라운드마다 모듈당 step 하나**만 실행하고 넘어간다. step은 블로킹하지 않는다 — 실행 후 의도를 반환하면 제어권이 곧장 pump로 돌아온다. 진짜 블로킹 작업은 step 안에서 돌리지 않고 스레드 풀로 넘긴다.

여러 flow가 한 스레드를 나눠 쓴다. 펌프가 모듈을 번갈아 방문하므로 어느 flow도 다른 flow를 막지 못한다.

한 라운드에서 *어떤 모듈도 실제 step을 돌리지 않았다면*(모든 활성 모듈이 async 대기 또는 `Sleep()` 대기 중) 펌프는 스핀하지 않고 짧게 잠든다. async 결과와 `Sleep()` 지연은 ms 단위라 1ms 폴링으로 충분하다 — 유휴 폴링이 CPU를 태우지 않는다.

---

## 5. 핵심 설계 결정 (그리고 *왜*)

> 이 섹션은 미래의 자신이 "이거 왜 이렇게 했지?" 묻기 전에 답해두는 게 목적. 각 항목에 `[유지]`(구버전부터 그대로) / `[개정]` / `[신규]` 표시.

### 5-1. step = 멤버 함수, flow = 본문 안 `NEXT(...)` 체인 `[유지]`

```cpp
StepResult OnCheckout_Validate() {
    if (!ok_) return Fail();
    return UF_NEXT(OnCheckout_Price);   // 다음 step 지정
}
```

각 step은 다음에 누구에게 넘길지를 *자기 본문 안*에서 결정한다. 흐름 정보가 한 군데에 모이지 않고 각 함수에 분산된다.

- step 추가/삭제/순서 변경이 *그 step만 건드리는* 한 줄 변경.
- 두 명이 다른 step에 동시 작업해도 git merge 충돌 면적이 거의 0.
- early-exit jump, retry loop 같은 분기가 step 본문 안에서 자연스러움.

### 5-2. 반환값으로 흐름 표현: `Stay / Sleep / Advance / Done / Fail` `[개정]`

```cpp
StepResult OnWaitReady() {
    if (!ready)  return Sleep(20ms);      // 20ms 뒤 이 step 다시 (페이스 폴링)
    if (timeout) return Fail();           // 흐름 abort
    return UF_NEXT(OnNextStep);           // 진행
}
```

framework가 커서 조작을 가져가고, 유저는 *의도만* 반환한다.

- `Stay()` — 같은 step을 *다음 라운드 즉시* 재실행. 펌프가 그만큼 hot-loop 한다. "지금 당장 다시"가 진짜 필요할 때만.
- `Sleep(d)` — 같은 step을 *`d` 경과 후* 재실행. `Stay()`의 페이스 버전. 조건 폴링("준비됐나 20ms마다 확인")과 모션 페이싱("16ms마다 한 프레임 전진")의 올바른 도구. 펌프가 그동안 스핀하지 않고 잠든다.
- `Advance / Done / Fail` — 진행 / 정상 종료 / 중단.

**왜 `Sleep`을 추가했나 `[신규]`**: 구버전은 `Stay()`가 유일한 폴링 도구였는데, 조건이 충족될 때까지 `Stay()`로 도는 루프는 펌프를 100% CPU로 busy-spin 시킨다. 외부 이벤트를 기다리거나(메시지 도착) 실시간 모션을 페이싱하는(시각화) 흐름에는 치명적이다. `Sleep(d)`는 "잠시 뒤 다시"를 일급 의도로 만들어, 런타임이 그 사이 펌프를 재울 수 있게 한다. `Stay()`는 "정말 즉시 재실행"이라는 좁은 용도로 남는다.

### 5-3. async 함수는 `static` 강제 + decay tuple `[유지]`

```cpp
UF_ASYNC(DoChargeCard, total_);
//        ^^^^^^^^^^^^ static 멤버 함수만 허용 (static_assert)

static ChargeResult DoChargeCard(double amount) {
    // this 없음. 멤버 변수 접근 불가. 필요한 데이터는 args로만.
}
```

worker 스레드에서 `this`를 만지면 메인 스레드와 race다. "조심하라"는 컨벤션은 사람이 어긴다 — 컴파일러로 강제하면 어길 수 없다. `static_assert(!is_member_function_pointer_v<...>)` 한 줄로 빌드 단계에서 차단. 추가로 `SubmitAsync`가 args를 `std::tuple<std::decay_t<Args>...>`로 복사/이동해 참조 우회도 막는다.

### 5-4. async 결과는 typed slot `[유지]`

```cpp
StepResult OnCheckout_ChargeDone() {
    auto r = AsyncResult<ChargeResult>();   // base가 보관, 유저 멤버 깨끗
    if (r.is_timeout() || r.failed()) return Fail();
    charged_ = r.value().amount;
    return UF_NEXT(OnCheckout_Confirm);
}
```

`std::future`/`wait_for`/`get`/`try-catch`는 유저 코드 0줄. base가 typed slot에 보관하고 continuation step에서 `AsyncResult<T>()`로 꺼낸다. async 디테일(timeout, 풀 선택, 조기 경보)은 `AsyncOpts`로 *그 호출 옆*에 박는다.

### 5-5. 자동 로깅 — CPU 시간 vs async 대기 분리, step ordinal `[유지]`

framework가 자동으로 잡는 것: step 진입/종료, step별 CPU 시간(메인 스레드 점유), 임계치 초과 시 `[WARN] SLOW CPU step`, async submit/completion/wait 시간, 흐름 trace 전체.

- step 번호는 *코드에서 제거, 로그에는 보존* — 런타임이 ordinal을 자동 부여하고 흐름별 평균 길이를 학습해 `reached #N/#M` 형식으로 출력한다.
- CPU 시간과 async 대기 시간은 *반드시 분리* 측정한다. 합치면 싱글스레드 보호 경보가 헛다리를 짚는다.

수동 로그는 항상 빠진 자리가 사고난 자리다. 그래서 유저 코드에 `LOG()` 한 줄 안 박아도 사고 분석이 되게 한다.

### 5-6. 런타임 은닉 — 정적 매니저, lazy 1개, `Init(n)` 옵트인 `[개정]`

`UniflowRuntime`을 유저가 들고 다니지 않는다. 유저가 *관리할 필요 없는* 객체이기 때문이다.

- 정적 매니저(Meyers 싱글톤)가 N개의 런타임을 소유한다. 런타임 1개 = pump 스레드 1개.
- `Init`을 호출하지 않으면, 첫 모듈/flow 사용 시점에 **런타임 1개로 lazy 초기화**된다. 단일 스레드만 쓰는 대다수 유저는 `Init`의 존재조차 모른다.
- 격리된 작업 도메인을 여러 개 굴리려면 `Init(n)`을 **옵트인**으로 호출한다. 통신 라이브러리에서 max socket 개수를 초기화 때 정하는 것과 같은 모델 — 고정 설정이다.
- `Init(n)`은 **첫 모듈/런타임이 만들어지기 전에** 불러야 한다. 이미 lazy 초기화가 일어난 뒤 호출하면 assert/throw — "런타임 개수는 첫 사용 전에 정하라"를 구조적으로 강제.
- 모듈은 소속 런타임을 **0-기반 정수 인덱스**로 고른다. 기본값 0. 이름이 아니라 숫자인 이유: 이름은 유저에게 불필요한 결정(작명·관리 대상)을 떠넘긴다. 개수는 init에서 한 번 박히는 고정 설정이라 cascading edit 문제도 없다.

**왜 싱글톤이 아니라 매니저+N개인가:** 완벽히 격리된 작업들은 별도 런타임(별도 스레드)으로 굴리는 게 효율적이다. 하드 싱글톤이면 테스트 격리와 도메인 분리가 불가능하다. 그러나 런타임 *객체*를 유저가 쥐고 있을 이유도 없다 — 그래서 "매니저가 소유, 유저는 인덱스로만 지정"으로 절충한다.

**파생 결과 (구현 시 결정 필요):** 풀(executor)도 런타임처럼 숨겨야 한다. 기본 executor를 lazy로 만들고, named 풀이 필요하면 `Init`처럼 첫 사용 전 옵트인 등록하는 방향을 권장한다. → §9 미해결 항목.

### 5-7. 함수 포인터 진입점 + 인자 전달 `[개정]`

flow는 매직스트링이 아니라 진입 step 함수 포인터로 시작한다. `Start`는 인자를 받아 진입 step에 그대로 전달한다.

```cpp
StepResult OnCheckout_Begin(int items, double unit_price) {   // 진입 step만 인자를 받음
    items_ = items; unit_price_ = unit_price;
    return UF_NEXT(OnCheckout_Validate);                      // 이후 step은 StepResult()
}

// 호출:
OrderModule::inst().Start(&OrderModule::OnCheckout_Begin, 3, 25.0);
```

- 런타임은 `Start` 시점에 `fn + args`를 decay-복사해 묶어두고, flow 첫 실행 때 그것으로 진입 step을 한 번 호출한다. 진입 step만 시그니처가 자유롭고, 이후 step은 전부 `StepResult()`.
- 컴파일 타임 검증(오타 시 빌드 에러), 리네임 안전, grep 가능.
- `UF_ENTRY`/`DefineFlow`/flow 이름 레지스트리가 통째로 사라진다 — 생성자의 등록 리스트가 없어진다. flow는 "그 진입 함수에서 시작하는 step 체인"으로 *암묵적으로* 정의된다.
- 진입 step의 *이름*은 로그에서 `(entry)`로 표기된다. `Start`는 매크로가 아닌 일반 함수라 함수 포인터를 문자열화할 수 없고(매크로화는 grep 가능성 때문에 기각), 함수 포인터만으로 이름을 얻을 표준 방법은 없다. 이후 step들은 `UF_NEXT(#fn)`이 이름을 잡아주므로 영향 없다.

**왜:** 구버전은 매직넘버를 없앤다면서 정작 flow 이름을 매직스트링으로 남겨뒀다. 함수 포인터 진입은 그 모순을 없애고, 인자 전달은 "멤버에 값 세팅 후 Start" 래퍼 함수를 통째로 제거해 진입 스타일을 하나로 통일한다.

### 5-8. 진입 step은 public, 중간 step은 private `[신규]`

```cpp
public:
    StepResult OnProbe_Begin(int max_polls);   // 외부 진입점
private:
    StepResult OnProbe_WaitReady();            // 내부 step
    StepResult OnProbe_Done();
```

외부에서 `&S::OnProbe_Begin`을 쓰려면(클래스 밖에서 멤버 포인터를 만들려면) 그 step이 public이어야 한다. 중간 step은 클래스 내부의 `UF_NEXT`에서만 참조되므로 private이면 된다.

부수 효과가 본질이다 — **접근 제어자가 곧 흐름 구조 문서**가 된다. public step = 진입점, private step = 내부 단계. 한 모듈이 외부에 노출하는 진입점이 어느 함수인지 한눈에 보인다.

### 5-9. 모듈 정체성 — 익명 1개 + 명명 N개 `[신규]`

같은 타입 모듈이 여러 개일 때 로그가 헷갈리는 문제를, *멀티 인스턴스 금지*가 아니라 *비용을 복잡도에 비례시켜* 푼다.

> **규칙:** 한 타입의 *익명*(이름 없는) 인스턴스는 0 또는 1개만 허용. 같은 타입의 두 번째 인스턴스부터는 이름을 강제한다. 위반 시 생성 시점에 assert/throw.

- 단일 인스턴스(흔한 경우 — 디스패처, 호스트별 핸들러): 이름 0개. `UF_USES_UNIFLOW(Cls)` / `UF_SINGLETON(Cls)` 매크로가 받은 클래스명 토큰을 기본 인스턴스 이름으로 쓴다. 로그 태그 `[Cls]`.
- 멀티 인스턴스(로봇 팔, 다수 호스트): 이름 강제. `Arm left{"left"}, right{"right"}` → 로그 `[Arm:left]`, `[Arm:right]`.
- 이름은 **타입 단위로만** 고유하면 된다. 레지스트리 키는 `(타입, 이름)` 쌍 — `Arm`의 `"left"`와 `Foot`의 `"left"`는 충돌하지 않는다. CRTP(`Uniflow<Arm>`)라 타입마다 별도 static 레지스트리가 자연히 생긴다.

결과: 이름 없는 같은 타입 인스턴스 2개는 *애초에 존재할 수 없다*. 인스턴스가 둘 이상이면 반드시 고유 이름이 붙어 있으므로 로그는 절대 모호하지 않다. 로그 혼동 비용(=작명)은 멀티 인스턴스를 실제로 만들 때만 지불된다 — 단일 인스턴스 유저는 한 푼도 안 낸다.

**멀티 인스턴스를 금지하지 않는 이유:** 로봇 제어에서 `Arm`을 정적 타입 하나로 강제하면 Left/Right Arm을 위해 클래스 2벌(로직 중복)을 써야 한다. 멀티 인스턴스면 `Arm` 클래스 하나에 인스턴스 2개. 같은 프로토콜을 쓰는 호스트 50개도 `HostHandler` 클래스 1개 × 인스턴스 50개지 클래스 50개가 아니다. "행동은 같고 정체성/상태만 다른" 케이스는 드물지 않다.

### 5-10. 모듈 간 접근 — `UF_SINGLETON` / `GetInst`, *같은 런타임 안에서만* `[신규]`

서로 다른 모듈이 상태를 읽거나 트리거를 주고받는 경로를 명시적으로 제공한다.

```cpp
// 단일 인스턴스
class OrderRouter : public uniflow::Uniflow<OrderRouter> {
    UF_SINGLETON(OrderRouter);          // 자동 생성·등록 + inst() + 2개째 생성 금지
};
OrderRouter::inst().GetQueueDepth();

// 다중 인스턴스
class Arm : public uniflow::Uniflow<Arm> { UF_USES_UNIFLOW(Arm); };
Arm left{"left"}, right{"right"};
Arm::GetInst("left").GetAngle();
```

`Cls::inst()`는 익명 인스턴스를, `Cls::GetInst("name")`은 명명 인스턴스를 같은 `(타입,이름)` 레지스트리에서 꺼낸다. `inst()`는 `GetInst()`(익명)의 단축형.

**보통의 멀티스레드 코드에서 전역 `Foo::inst()`는 race의 온상이라 안티패턴이다. 그러나 이 프레임워크에서는 정공법이다** — 같은 런타임의 모든 모듈은 같은 pump 스레드 하나에서 협동 실행되므로, 모듈 A의 step 안에서 `ModuleB::inst().current_pos()`를 읽는 건 같은 스레드 내 접근이라 완벽히 안전하다. "공유 자원 접근이 무섭다"는 게 이 프레임워크를 쓰는 이유인데, 바로 그 무서움이 같은 런타임 안에서는 사라진다.

**반드시 막아야 할 구멍:** §5-6의 멀티 런타임과 충돌한다. `ModuleA`가 런타임 0, `ModuleB`가 런타임 1이면 서로 *다른 pump 스레드*다. A의 step에서 `ModuleB::inst()`를 만지면 다시 크로스 스레드 race다. 싱글스레드 보장은 *같은 런타임 안에서만* 성립한다.

- 디버그 빌드에서 `inst()`/`GetInst()`는 "호출자 모듈의 런타임 == 대상 모듈의 런타임"을 assert 해야 한다.
- 문서에 "크로스 런타임 접근 금지"를 크게 박는다.

**모듈 간 접근 컨벤션:** "상태 읽기 + 트리거"까지만. getter(`current_pos()`)나 트리거(`B::inst().Start(&B::OnDoThing, args)`)는 OK. A가 B의 step 커서를 직접 조작하는 건 금지 — flow는 자기 step만 운전한다.

---

## 6. 어휘 / 명명 규약

| 용어 | 의미 |
|---|---|
| **step** | 실행 단위. 멤버 함수 1개 = step 1개 |
| **flow** | 진입 step 함수에서 시작하는 step 체인. `Start(&S::OnX_Begin, ...)`로 진입 |
| **runtime** | pump 루프 1개 + 스레드 1개. 정적 매니저가 N개 소유, 유저는 인덱스로 지정 |
| **module** | `Uniflow<Derived>`를 상속한 클래스의 인스턴스. `(타입,이름)`으로 식별 |
| **ordinal** | flow 시작 이후 몇 번째 step 전환인가 (로깅용 카운터) |
| **executor** | 스레드풀 추상화 |
| **observer** | 모든 step 이벤트의 sink (로그/메트릭/테스트 hook) |

### 클래스/타입
- `uniflow::Uniflow<Derived>` — CRTP base. `StepResult` / `StepFn`은 그 안의 멤버 타입
- `uniflow::StepAction` (enum: Stay/Wait/Advance/Done/Fail)
- `uniflow::AsyncOpts`, `uniflow::AsyncRef<T>`
- `uniflow::Config` — slow-CPU 임계치 / idle sleep
- `uniflow::IExecutor`, `uniflow::StdThreadPool`, `uniflow::InlineExecutor`, `uniflow::BSThreadPoolExecutor`
- `uniflow::IUniflowObserver`, `uniflow::ConsoleObserver`
- `uniflow::TraceEntry`, `uniflow::FlowStats`

### 자유 함수 (런타임 은닉의 결과 — 모두 옵트인, 첫 모듈 생성 전)
- `uniflow::Init(n)` — 런타임 개수 지정 (기본 1, lazy)
- `uniflow::Configure(cfg)` — 전역 `Config` 설정
- `uniflow::RegisterExecutor(name, pool)` — named 풀 등록 (`"default"`는 자동 생성)
- `uniflow::SetObserver(obs)` — 기본 `ConsoleObserver` 교체

### step이 반환하는 헬퍼 (base가 제공)
- `Stay()` / `Sleep(d)` / `Done()` / `Fail()` / `Next(...)` (`UF_NEXT`가 래핑)
- `AsyncResult<T>()` — async 결과 수령

### 매크로 (모두 `UF_` 접두사)
- `UF_USES_UNIFLOW(Cls)` — 멀티 인스턴스 가능 클래스에. base friend + 클래스명 + `using S` + base 생성자 상속
- `UF_SINGLETON(Cls)` — 단일 인스턴스 클래스에. 위 + `inst()` + private 기본 생성자(2개째 생성 = 컴파일 에러). `UF_USES_UNIFLOW`와 둘 중 하나만
- `UF_NEXT(fn)` — step 함수 안에서 다음 step 지정
- `UF_ASYNC(fn, args...)` — 풀에 static fn 던지기 (default opts)
- `UF_ASYNC_OPT(fn, opts, args...)` — opts 명시

`Cls::inst()` / `Cls::GetInst("name")`는 매크로가 아니라 base가 제공하는 static 멤버. 두 매크로 모두 `using S = Cls;`를 제공하므로 유저가 따로 쓰지 않는다. 매크로는 `public:`/`private:` 구획을 바꾸므로 클래스 본문 *맨 위*에 두고, 진입 step 앞에 `public:`을 다시 쓴다.

### 코드 컨벤션
- step 함수 이름: `On<FlowName>_<Purpose>` (예: `OnCheckout_Validate`)
- async 결과 받는 step: `On<FlowName>_<TaskName>Done` (예: `OnCheckout_ChargeDone`)
- 진입 step은 `public`, 중간 step은 `private`
- 멤버 변수: `trailing_underscore_`

---

## 7. 유저 코드 모양 (목표)

```cpp
#include "uniflow.hpp"

// 단일 인스턴스 모듈 — 의례 0. Init 불필요, 인스턴스 작명 불필요.
class ReadyProbe : public uniflow::Uniflow<ReadyProbe> {
    UF_SINGLETON(ReadyProbe);            // S·friend·inst() 제공 + 2개째 금지

public:
    StepResult OnProbe_Begin(int max_polls) {   // 진입 step: public, 인자 받음
        max_polls_ = max_polls;
        polls_     = 0;
        return UF_NEXT(OnProbe_WaitReady);
    }

private:
    StepResult OnProbe_WaitReady() {            // 중간 step: private
        if (++polls_ < max_polls_) return Sleep(20ms); // 20ms마다 페이스 폴링
        return UF_NEXT(OnProbe_Done);
    }
    StepResult OnProbe_Done() { return Done(); }

    int polls_ = 0, max_polls_ = 0;
};

int main() {
    // Init() 호출 없음 → 런타임 1개로 lazy 초기화
    ReadyProbe::inst().Start(&ReadyProbe::OnProbe_Begin, 3);   // 매직스트링·래퍼·rt 객체 없음
    ReadyProbe::inst().Wait();                                 // flow 끝까지 블록
    return 0;
}
```

멀티 인스턴스 모양:

```cpp
class Arm : public uniflow::Uniflow<Arm> {
    UF_USES_UNIFLOW(Arm);                          // S·friend·생성자 상속 제공
public:
    StepResult OnMove_Begin(double target_angle) { /* ... */ }
};

Arm left{"left"}, right{"right"};                  // 2개째부터 이름 강제
left.Start(&Arm::OnMove_Begin, 90.0);
Arm::GetInst("right").Start(&Arm::OnMove_Begin, -90.0);
```

---

## 8. 거부한 대안 / 결정 이력

이 디자인은 한 번에 나오지 않았다. 거부의 *이유*가 디자인의 본질이다.

### flow 표현 (구버전부터)
| 대안 | 왜 거부 |
|---|---|
| switch + 매직넘버 (`case 100, 101…`) | 매직넘버 hard-code, step 추가 시 cascade, merge 충돌 |
| switch + constexpr offset (`OFFSET+N`) | base는 한 군데지만 `+1,+2` 손번호 cascade 여전 |
| nested switch (group, offset) | 매직넘버는 사라지나 한 함수에 로직 집중 → 동시 편집 충돌 |
| 생성자 `AddStep(name,&fn)` 등록 리스트 | step 한 줄 추가가 등록 리스트 + 함수 본문 2군데 수정 |
| `Step(name,[this]{...})` 람다 등록 | 람다 디버깅 불편, 팀 미친숙 → **유저 명시 거부** |
| C++20 coroutine | 코드는 가장 깔끔하나 boilerplate 100~150줄 + 코루틴 frame 디버깅 비용 → 보류 |
| → **본문 안 `UF_NEXT(...)` 체인** | **채택.** 위치 의존성 0, 머지 충돌 면적 최소 |

### async 패턴 (구버전부터)
future 멤버 → 거부(오염), continuation 인자 → 거부(시그니처 통일 불가), 출력 인자 → 거부(예외 표현 불가), **typed slot + `AsyncResult<T>()`** → 채택. async 함수 안전은 `static` + decay tuple로 컴파일 강제.

### 인터페이스 (이번 개정)
| 대안 | 왜 거부 |
|---|---|
| `UniflowRuntime`을 하드 싱글톤으로 | 테스트 격리·도메인 분리 불가 → 매니저 + N개 |
| 유저가 `UniflowRuntime` 객체 보유 | 관리할 필요 없는 객체를 떠안김 → 숨김 |
| 런타임을 이름으로 식별 | 불필요한 작명 결정을 유저에게 떠넘김 → 0-기반 정수 인덱스 |
| `Init()` 호출 필수 | 단일 스레드 유저에게 불필요한 의례 → lazy 기본 1개, `Init`은 옵트인 |
| flow를 매직스트링으로 (`Start("X")`) | "매직넘버 제거" 철학과 모순 → 함수 포인터 진입점 |
| 모든 step을 private + `Request*()` 래퍼 | `Start`가 인자를 받으면 래퍼 불필요 → 진입 step public |
| 같은 타입 멀티 인스턴스 금지(정적 강제) | 로봇 팔·다수 호스트 등 흔한 케이스를 죽임 → 익명 1 + 명명 N |
| 멀티 인스턴스용 별도 클래스(`UniflowO`) | 학습 곡선·문서 2벌, API 부채 → 단일 클래스 + 작명 정책 |

이 결정들의 *반대 방향*으로 가려면 위 이유들을 먼저 무력화하는 새 사실이 있어야 한다.

---

## 9. 현재 파일 구성 / 미해결 항목

### 파일
| 파일 | 역할 |
|---|---|
| [cpp/uniflow.hpp](../cpp/uniflow.hpp) | 프레임워크 전체 (header-only, C++17). §2/§5 개정 컨셉 반영 완료 |
| [include/BS_thread_pool.hpp](include/BS_thread_pool.hpp) | 벤더링한 BS::thread_pool v4.1.0. 선택 — `UNIFLOW_USE_BS_THREAD_POOL` 정의 시 |
| [cpp/examples/message_dispatch/](../cpp/examples/message_dispatch/) | 예제 1 — 학생/스케줄러 메시지 분배 (콘솔). step 체인 + async + 다중 클라이언트 |
| [cpp/examples/cnc_pickers/](../cpp/examples/cnc_pickers/) | 예제 2 — CNC 피커 (Win32 시각화 + 콘솔 폴백). 1클래스 2인스턴스 + 충돌 회피 |
| [notes/make_concept.hpp](notes/make_concept.hpp) | 최초 브레인스토밍 파일. 역사적 기록 |
| [temp/](temp/) | 구버전 튜토리얼·데모·VS 프로젝트 보관. 참조하지 않음 |
| [DESIGN.md](DESIGN.md) | 이 파일 |

### 구현하며 확정된 것 (§5 외 추가 결정)
- **`Sleep(d)` / `StepAction::Wait`** — §5-2 참조. 페이스 폴링·모션 페이싱용. 구버전 `Stay()` 단독으로는 폴링 루프가 펌프를 100% busy-spin 시켰다.
- **펌프 무스핀** — 한 라운드에서 실제 step이 하나도 안 돌면(전부 async/Sleep 대기) 펌프가 `idle_sleep`만큼 잔다. `ExecuteOnce`가 "step을 돌렸는가" bool을 반환.
- **executor 은닉** — `Hub`가 전역 executor 맵 소유. `"default"` 풀 lazy 생성, named 풀은 `RegisterExecutor` 옵트인.
- **pump 시작** — 첫 모듈 생성 시 `Hub`가 모든 런타임의 pump 스레드를 기동. `main`은 `Module::Wait()`로 블록.
- **`UF_SINGLETON` 생성** — `inst()` 안 Meyers static. 첫 `inst()` 호출 시 생성·등록. 런타임은 항상 0.
- **모듈 수명 계약** — 유저 소유 모듈의 base 소멸자가 런타임에서 등록 해제(라운드 경계까지 블록). flow 진행 중 소멸은 `assert`로 차단 — "idle일 때만 파괴" 계약.

### 미해결 / 다음 결정
- [ ] **`UF_SINGLETON`의 런타임 지정** — 현재 항상 0. 다른 런타임에 둘 방법이 필요한지.
- [ ] **에러 reason** — `Fail()`이 흐름만 종료. `Fail(reason)`으로 정보 보존.
- [ ] **외부 결과 받기** — `Start`가 future 반환 또는 callback 등록.
- [ ] **Cancellation** — 협력형 캔슬 (`shared_ptr<atomic<bool>>`).
- [ ] **명령 mailbox** — 모듈이 바쁠 때 외부 트리거를 큐잉 (예제 1은 수동 mailbox로 우회).
- [ ] **다수 in-flight async** — 현재 객체당 1슬롯. named slot 필요 시.
- [ ] **테스트 인프라** — `InlineExecutor` + no-op observer로 deterministic 검증. 프레임워크 결정 (catch2/doctest/gtest).
- [ ] **VS2022 솔루션 / 빌드 스크립트** — 예제 헤더의 `cl`/`g++` 명령으로 빌드 가능. 정식 프로젝트 파일은 미정.

---

## 10. 다음 세션 진입 가이드

이 문서를 읽고 → §5 결정 10개 + §9 "구현하며 확정된 것" → [cpp/uniflow.hpp](../cpp/uniflow.hpp) 헤더 주석 → [cpp/examples/](../cpp/examples/)의 두 예제 순으로 보면 컨텍스트가 복원된다.

막힐 때마다 *왜 막히는지*를 §8 결정 이력과 대조해 디자인이 안 맞는 건지 구현 디테일인지 구분한다.
