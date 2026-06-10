# uniflow 튜토리얼

> 🌐 언어: **한국어** | [English](TUTORIAL.md)

한 챕터에 한 개념씩. 챕터마다 *완전히 컴파일되고 도는* 코드 한 덩이를 줍니다. 12개 챕터 + 마지막 오케스트레이션까지 끝나면 [examples/](examples/) 안의 코드가 전부 자연스럽게 읽힙니다.

> 처음이라면 [README](../README.kr.md)의 3개 퀵 튜토리얼(라운드로빈 / async / observer)을 먼저 보고 오면 좋습니다. 이 문서는 그 다음 단계입니다.

각 챕터의 코드는 `tutorial/chapNN.cpp` 라고 가정하고 적어둡니다. 빌드는:

```powershell
cl /std:c++17 /EHsc /I . tutorial\chap01.cpp /Fe:chap01.exe
```

또는 g++:
```bash
g++ -std=c++17 -O2 -pthread -I . tutorial/chap01.cpp -o chap01
```

> 💡 모든 챕터는 `#include "uniflow.hpp"` 한 줄로 시작합니다. 추가 의존성 없음.

---

## 챕터 1. 한 step짜리 모듈

가장 작은 uniflow 모듈은 - step 함수 하나, Done() 한 번. 이게 끝입니다.

```cpp
#include "uniflow.hpp"
#include <iostream>

class Hello : public uniflow::Uniflow<Hello> {
    UF_UNIFLOW_IMPLEMENT(Hello);
public:
    explicit Hello(uniflow::Runtime& rt)
        : uniflow::Uniflow<Hello>(rt) {}

    StepResult OnHello_Begin() {
        std::cout << "hello from a step\n";
        return Done();
    }
};

int main() {
    uniflow::Runtime rt;
    Hello h{rt};
    UF_START_FLOW(h, OnHello_Begin);
    h.WaitUntilIdle();
}
```

콘솔에 뜨는 것:
```
[Hello         ] FLOW START  caller=chap01.cpp:18 main()
hello from a step
[Hello         ] (entry)      ...  #00 elapsed=0.05ms  tick x1 ...
[Hello         ] FLOW END  DONE  steps=#00 ...
```

**무슨 일이 있었나**
- `Runtime`이 펌프 스레드 1개를 띄움
- `Hello h{rt};` - 모듈을 매다는 순간 펌프가 매 라운드 `h`를 방문하기 시작
- `UF_START_FLOW(h, OnHello_Begin)` - flow를 켬. 다음 라운드부터 step 호출
- step이 `Done()` 돌려줘서 모듈은 idle 상태로 복귀

**기억할 거 하나** - step 함수 안에서 `std::this_thread::sleep_for` 같은 거 *절대* 쓰지 마세요. 그 시간동안 펌프 전체가 멈춥니다. 기다리고 싶으면 `Stay()` (다음 챕터) 또는 `UF_ASYNC` (챕터 6).

---

## 챕터 2. step 체인 (Next)

step 한 개로는 별거 못 합니다. 보통은 N개를 줄줄이 잇습니다. `UF_NEXT(다음_step)` 으로 advance.

```cpp
class Greet : public uniflow::Uniflow<Greet> {
    UF_UNIFLOW_IMPLEMENT(Greet);
public:
    explicit Greet(uniflow::Runtime& rt)
        : uniflow::Uniflow<Greet>(rt) {}

    StepResult OnGreet_Begin() {
        std::cout << "1. 안녕하세요\n";
        return UF_NEXT(OnGreet_Middle);
    }

private:
    StepResult OnGreet_Middle() {
        std::cout << "2. 반가워요\n";
        return UF_NEXT(OnGreet_End);
    }
    StepResult OnGreet_End() {
        std::cout << "3. 또 봐요\n";
        return Done();
    }
};

int main() {
    uniflow::Runtime rt;
    Greet g{rt};
    UF_START_FLOW(g, OnGreet_Begin);
    g.WaitUntilIdle();
}
```

**왜 굳이 step 함수로 자르나** - 콜백/async를 끼울 자리가 *바로 step 경계* 이기 때문입니다. 어떤 step에서 1초짜리 일을 시작했으면, 그 자리에서 `UF_ASYNC` 던지고 다음 step에서 결과 받습니다. 일이 끝날 때까지 펌프는 다른 모듈들을 돌리고 있어요.

**진입 step은 public** - 외부에서 `UF_START_FLOW`로 부르니까요. 중간 step은 private이 좋습니다 (이 모듈의 내부 흐름).

---

## 챕터 3. 폴링 (Stay)

조건이 만족될 때까지 같은 step을 다시 돌리고 싶으면 `Stay()`. 펌프는 stay_sleep_ms (기본 20ms) 만큼 쉬었다가 다시 와서 같은 step을 또 부릅니다.

```cpp
class WaitForFive : public uniflow::Uniflow<WaitForFive> {
    UF_UNIFLOW_IMPLEMENT(WaitForFive);
public:
    explicit WaitForFive(uniflow::Runtime& rt)
        : uniflow::Uniflow<WaitForFive>(rt) {}

    StepResult OnCount_Begin() {
        started_at_ = uniflow::Clock::now();
        return UF_NEXT(OnCount_Tick);
    }

private:
    StepResult OnCount_Tick() {
        auto elapsed = uniflow::Clock::now() - started_at_;
        if (elapsed >= std::chrono::seconds(5)) {
            std::cout << "5초 경과\n";
            return Done();
        }
        // 아직. 다음 라운드에서 또 와요.
        return Stay();
    }

    uniflow::TimePoint started_at_;
};
```

**`Stay()` 의 진짜 쓸모** - 외부 하드웨어 플래그 폴링, 다른 모듈의 상태 변화 기다리기, "조건 충족될 때까지 대기" 같은 거. *블로킹 일* (네트워크/디스크/긴 계산) 은 `Stay()` 가 아니라 `UF_ASYNC` 입니다.

### `UFTimer` - step 안에서 시간 재기

위 예제는 스톱워치를 손으로 만들었습니다 - `TimePoint` 멤버 + `Clock::now()` 계산. 워낙 흔한 패턴이라 헬퍼가 있습니다. 그리고 이건 보기보다 중요합니다: **단일 스레드 협력형에선 절대 `sleep` 을 못 씁니다** - 펌프 전체가 멈추니까요 - 그래서 "N ms 대기", "T 후 타임아웃", "D 동안 안정화" 같은 *모든* 시간 처리가 `Stay()` 라운드마다 폴링하는 타이머로 표현됩니다. 여기선 시간이 원래 이렇게 동작합니다. 익숙해지면 좋습니다.

`uniflow::UFTimer` 는 읽기 3종을 가진 스톱워치입니다:

```cpp
uniflow::UFTimer t;        // 생성 시 무장; 언제든 t.Restart() 로 재무장

t.Elapsed();               // 무장 후 경과 Duration
t.Passed(2s);            // bool: 무장 후 2s 지났나?
t.HeldFor(cond, 50ms);      // bool: cond 가 50ms 동안 연속으로 참이었나?
```

- **`Passed(d)`** - 무장 후 그 시간이 지났나? 위 5초 대기가 이렇게 줄어듭니다:

```cpp
StepResult OnCount_Tick() {
    if (t_.Passed(5s)) { std::cout << "5초 경과\n"; return Done(); }
    return Stay();                       // 아직 - 다음 라운드에 또 폴링
}
// 멤버: uniflow::UFTimer t_;  (OnCount_Begin 에서 t_.Restart() 로 무장)
```

- **`HeldFor(cond, d)`** - *안정화(settling)* / 디바운스. `cond` 가 `d` 동안 *연속으로* 참이어야 true; 한 번이라도 false면 카운트가 리셋. 진짜 안정되기 전에 튀는(bounce) 하드웨어 플래그에: `if (t_.HeldFor(Hw::Ready(), 50ms)) ...`.

- **`Elapsed()`** - 원시 경과 시간. 모션 페이싱이나 진행률에: `double frac = Elapsed() / total;`.

**어디에 두나.** step 을 넘나드는 타이머(step A에서 무장, step B에서 체크)는 step보다 오래 살아야 하니 **멤버**로 두거나, `UF_NEXT(NextStep, uniflow::UFTimer{})` 로 넘겨 `Stay()` 루프 전체가 한 타이머를 공유하게 합니다. 지역 변수 타이머는 한 step 본문 안에서만 잽니다.

> **배속·freeze 되는 하나의 시계.** 타이머를 런타임에 바인딩하면 - `uniflow::UFTimer t{rt.clock()}` - 그 런타임의 *가상* 시계를 따릅니다. `rt.clock().SetScale(10)` 은 전체 흐름을 10배속 재생, `rt.clock().Freeze()` / `.Resume()` 은 모든 논리 타임아웃을 정지(예: e-stop 중 3초 타임아웃이 라인 멈춤 동안 안 터지게). 그냥 `UFTimer{}` 는 실제 시간. async/IO 데드라인은 배속과 무관하게 항상 실제 시간입니다.

조건을 폴링하되 **영영 안 오면 복구 step 으로 빠지는** 경우는 챕터 7의 `UF_STAY_UNTIL` 을 보세요.

---

## 챕터 4. Describe - 디버깅용 한 줄

각 step이 "지금 뭘 하고 있는지" observer/로그에 짧게 적어주면 디버깅이 편합니다. `Describe(...)` 하나가 그 줄을 만듭니다.

```cpp
StepResult OnLoad_WaitAtSource() {
    if (GlobalEnv::Stop()) return Done();
    x_axis_.Update(GlobalGeometry::kXSpeed_mm_per_s);
    Describe("approaching A");          // <-- 이 줄
    if (x_axis_.InPosition()) return UF_NEXT(OnLoad_CmdLowerToPick);
    return Stay();
}
```

콘솔 로그가 이렇게 나옵니다:
```
[UF_LoadPicker ] OnLoad_WaitAtSource    approaching A    #01 elapsed=42ms  ...
```

`Describe`는 가변 인자를 받아서 `<<` 으로 이어붙입니다:
```cpp
Describe("parked at A-gap: stage=", ToString(stage_state), " partner_in_B=", partner_in_b);
```

step 전환할 때 마지막으로 적힌 description이 한 번 콘솔에 찍히고 비워집니다. 부담 없이 새 step마다 새로 적으면 됩니다.

---

## 챕터 5. 모듈 여럿이 한 Runtime을 공유

핵심: **같은 Runtime에 매단 모듈들은 같은 펌프 스레드에서 돕니다**. 모듈 사이 공유 자원에 락이 필요 없습니다.

```cpp
namespace shared { std::ostringstream g_log; int g_turn = 0; }

class Pinger : public uniflow::Uniflow<Pinger> {
    UF_UNIFLOW_IMPLEMENT(Pinger);
public:
    StepResult OnPing_Begin() { return UF_NEXT(OnPing_Loop); }
private:
    StepResult OnPing_Loop() {
        if (count_ <= 0) return Done();
        if (shared::g_turn != 0) return Stay();  // 차례 기다림
        shared::g_log << "ping ";
        shared::g_turn = 1;
        --count_;
        return Stay();
    }
    int count_ = 5;
};

class Ponger : public uniflow::Uniflow<Ponger> {
    UF_UNIFLOW_IMPLEMENT(Ponger);
public:
    StepResult OnPong_Begin() { return UF_NEXT(OnPong_Loop); }
private:
    StepResult OnPong_Loop() {
        if (count_ <= 0) return Done();
        if (shared::g_turn != 1) return Stay();
        shared::g_log << "pong\n";
        shared::g_turn = 0;
        --count_;
        return Stay();
    }
    int count_ = 5;
};

int main() {
    uniflow::Runtime rt;
    Pinger p{rt};
    Ponger q{rt};
    UF_START_FLOW(p, OnPing_Begin);
    UF_START_FLOW(q, OnPong_Begin);
    p.WaitUntilIdle();
    q.WaitUntilIdle();
    std::cout << shared::g_log.str();
}
```

출력:
```
ping pong
ping pong
ping pong
ping pong
ping pong
```

`g_log`, `g_turn` 어디에도 mutex 없습니다 - 둘 다 같은 펌프 스레드 위에서만 만져지니까요. (예제 1 [shared_ostream](examples/shared_ostream/) 이 이 패턴의 완성판입니다.)

**경고** - Runtime을 *둘* 만들면 펌프가 둘이 됩니다. 그땐 두 펌프 사이의 공유 자원이 다시 멀티스레드 문제가 됩니다. 보통은 Runtime 하나로 충분하고, 정말 나눠야 할 때 둘 사이를 락 없이 잇는 방법(`Post` / `Link`)은 챕터 9에서 다룹니다.

---

## 챕터 6. 진짜 블로킹 작업 - UF_ASYNC

step 본문 안에서 5초짜리 HTTP 요청을 직접 호출하면? 그 5초 동안 펌프가 멈춥니다 -> 다른 모듈도 다 멈춥니다. 절대 안 됩니다.

해법: `UF_ASYNC(static_fn, args...)` 로 풀에 던지고 *다음 step*에서 결과를 받습니다.

```cpp
class Fetcher : public uniflow::Uniflow<Fetcher> {
    UF_UNIFLOW_IMPLEMENT(Fetcher);
public:
    explicit Fetcher(uniflow::Runtime& rt)
        : uniflow::Uniflow<Fetcher>(rt) {}

    StepResult OnFetch_Begin(std::string url) {
        url_ = std::move(url);
        UF_ASYNC(DoHttpGet, url_);              // 풀 스레드로 던짐
        return UF_NEXT(OnFetch_Done);            // 다음 step에서 결과 수신
    }

private:
    StepResult OnFetch_Done() {
        auto r = AsyncResult<std::string>();
        if (r.failed()) {
            std::cout << "fetch failed\n";
            return Fail();
        }
        std::cout << "got " << r.value().size() << " bytes\n";
        return Done();
    }

    // UF_ASYNC 타겟은 무조건 STATIC. this 멤버 못 만지게 막혀 있어요.
    static std::string DoHttpGet(std::string url) {
        // ... 진짜 HTTP 호출이 여기서 일어남, 풀 스레드 위 ...
        return "<html>...</html>";
    }

    std::string url_;
};
```

**왜 static만 되나** - 풀 스레드에서 도는 동안 모듈은 펌프 스레드 위에서 다른 step을 돌릴 수도 있는데, 거기서 멤버 변수를 만지면 race. 그래서 static을 강제하고, 필요한 데이터는 *인자로 복사해서* 풀로 넘깁니다. (`url_` 멤버를 인자로 한 번 복사함.)

결과는 `AsyncResult<T>()` 로 받습니다 - *연속 step 안에서만* 유효합니다. 한 step이 두 개를 동시에 띄울 수는 없어요(현재 in-flight는 한 번에 하나).

**관찰** - 콘솔에 자동으로 찍힙니다:
```
[Fetcher       ] (entry)                ASYNC SUBMIT  DoHttpGet
[Fetcher       ] (entry) -> OnFetch_Done ...
[Fetcher       ]                        ASYNC DONE    DoHttpGet  wait=120.4ms
[Fetcher       ] OnFetch_Done           got 1024 bytes  #01 elapsed=120.5ms
```

---

## 챕터 7. 타임아웃 / 에러 / 실패 처리

`UF_ASYNC_TIMEOUT(fn, dur, args...)` - 데드라인 붙은 async.

```cpp
using namespace std::chrono_literals;

StepResult OnQuery_Begin() {
    UF_ASYNC_TIMEOUT(SlowApi, 2s, query_);   // 2초 안에 응답 못 받으면 timeout
    return UF_NEXT(OnQuery_After);
}

StepResult OnQuery_After() {
    auto r = AsyncResult<Response>();
    if (r.is_timeout()) {
        Describe("API didn't respond in 2s");
        return Fail();
    }
    if (r.failed()) {
        try { std::rethrow_exception(r.exception()); }
        catch (const std::exception& e) {
            Describe("API threw: ", e.what());
        }
        return Fail();
    }
    return UF_NEXT(OnQuery_Use, r.value());
}
```

**`AsyncRef<T>` 의 세 가지 상태**:
- `is_timeout()` true - 데드라인 넘김
- `failed()` true - 풀 스레드에서 예외 throw됨, `exception()` 에 ptr
- 둘 다 false - `value()` 가 유효

### 스텝 타임아웃: `UF_STAY_UNTIL` - 스텝 단위 `catch`

`UF_ASYNC_TIMEOUT` 은 async *잡* 이 늦어진 경우를 본다. 그런데 잡을 기다리는 게 아닐 때가 많다 - 하드웨어에 명령을 내려놓고 "완료" 플래그나 센서를 `Stay()` 로 폴링하는 경우. **그게 영영 안 오면?** 축이 끼이거나, 엔코더가 끊기거나, 밸브가 멈추면 - 맨 `Stay()` 루프는 *무한히* 폴링하고, 라인은 에러도 없이 조용히 멈춰 선다. 실제 장비에선 이게 최악이다.

`UF_STAY_UNTIL(dur, fn)` 은 **마감이 달린 `Stay()`** 다: 이 step 을 계속 폴링하되, step 에 *진입한 시점* 부터 `dur` 이 지나면 step `fn` 으로 빠진다. `fn` 이 곧 `catch` - 정해진 복구 경로로의 보장된 탈출구다.

전체 패턴 - 명령, 마감 걸린 대기, 복구:

```cpp
// 축을 목표 위치로 이동시키고, InPosition 보고를 기다린다.
StepResult OnMove_Command() {
    axis_.MoveTo(target_mm_);                    // 명령 발사 (논블로킹)
    Describe("moving to ", target_mm_, " mm");
    return UF_NEXT(OnMove_WaitInPos);
}

StepResult OnMove_WaitInPos() {
    if (axis_.InPosition())                      // 정상 경로
        return UF_NEXT(OnMove_Clamp);
    // 아직 이동 중 - 폴링하되 2s 넘게 멈춰있으면 포기
    return UF_STAY_UNTIL(2s, OnMove_Stalled);
}

// 대기 step 진입 후 2s 안에 InPosition 이 끝내 true 가 안 된 경우에만 도달.
// 흐름은 멈춰 설 수 없다 - 항상 정의된 어딘가로 도착한다.
StepResult OnMove_Stalled() {
    axis_.Abort();                               // 모션 정지
    Alarm("axis stalled before reaching target");
    return Fail();
}
```

`UF_STAY_UNTIL` 없이 `OnMove_WaitInPos` 가 맨 `Stay()` 를 반환했다면, 누군가 라인이 죽은 걸 알아챌 때까지 무한히 돈다. 이걸 쓰면 이동이 안 끝날 경우 `OnMove_Stalled` 에 *반드시* 도달한다 - 흐름은 늘 정의된 상태로 전진한다.

**복구 step 도 그냥 step 이라 어디로든 라우팅할 수 있다.** 아주 흔한 형태가 *재시도 후 포기* 다:

```cpp
StepResult OnMove_WaitInPos() {
    if (axis_.InPosition()) return UF_NEXT(OnMove_Clamp);
    return UF_STAY_UNTIL(2s, OnMove_Retry);
}

StepResult OnMove_Retry() {
    if (++attempts_ >= 3) {                       // 재시도 소진
        Alarm("axis failed to reach target after 3 tries");
        return Fail();
    }
    axis_.Abort();
    Describe("retry ", attempts_, "/3");
    return UF_NEXT(OnMove_Command);               // 재발행 -> 대기 step 재진입,
}                                                 // 2s 창이 다시 시작됨
```

`OnMove_Command` -> `OnMove_WaitInPos` 재진입은 *새 step 진입* 이라, 시도마다 2s 마감이 새로 시작된다 - 수동 타이머 관리가 필요 없다.

알아둘 세 가지:

- **마감은 `UF_STAY_UNTIL` 호출 시점이 아니라 step 진입 기준**이다. 매 폴링 틱마다 반환하지만 시계는 틱마다 리셋되지 않는다 - 2s 폴링하는 step 은 정확히 2s 에 타임아웃된다.
- **논리 시간이다.** 마감은 런타임 시계(챕터 3의 `rt.clock()`) 위에서 도므로 `Freeze()` 하면 멈추고(예: e-stop 중 2s 타임아웃이 안 터짐) `SetScale` 로 스케일된다. async/IO 데드라인은 실제 시간 유지.
- **타임아웃 둘, 역할 둘.** `UF_ASYNC_TIMEOUT` = "이 *잡*(UF_ASYNC)이 T 안에 끝나야"(다음 step 에서 `is_timeout()` 읽음). `UF_STAY_UNTIL` = "이 *step* 이 T 안에 진전돼야"(복구 step 으로 빠짐). 앞은 async 작업에, 뒤는 폴링 조건에.

챕터 3의 `HeldFor` 와도 자연스럽게 짝이 된다 - 플래그가 *안정* 되길 요구하되, 끝내 안 정착하면 빠지기:

```cpp
StepResult OnMove_WaitInPos(uniflow::UFTimer& t) {   // t 는 UF_NEXT(..., uniflow::UFTimer{}) 로 전달
    if (t.HeldFor(axis_.InPosition(), 50ms))         // 위치 도달 AND 50ms 안정
        return UF_NEXT(OnMove_Clamp);
    return UF_STAY_UNTIL(2s, OnMove_Stalled);         // 끝내 미정착 -> 복구
}
```

**step 본문에서 예외가 나면?** - 기본 동작은 *전체 종료* (`std::terminate`). 살리고 싶으면 모듈에 `CatchStepExceptions()`를 override 해서 `true` 돌려주세요:

```cpp
class SoftFail : public uniflow::Uniflow<SoftFail> {
    UF_UNIFLOW_IMPLEMENT(SoftFail);
public:
    bool CatchStepExceptions() const { return true; }
    // 이제 step에서 throw 나면 OnStepThrew -> flow Fail() 로 끝남, 펌프 살아남음
};
```

---

## 챕터 8. observer 갈아끼우기

기본 observer (`ConsoleObserver`)는 step 전환, async 시작/끝, slow-step alarm 등을 콘솔에 예쁘게 적어줍니다. 더 정교한 게 필요하면 - 파일로 동시 출력, 로그 서버 전송, 메트릭 카운터 - `IUniflowObserver` 를 상속한 새 observer를 만들어서 Runtime에 꽂으면 됩니다.

```cpp
class MyObserver : public uniflow::IUniflowObserver {
public:
    void OnStepChanged(std::string_view obj,
                       std::string_view prev_step, std::string_view next_step,
                       std::string_view description,
                       int step_ordinal, double elapsed_ms,
                       const uniflow::TickStats& step_ticks) override {
        // 내 양식으로 적기, 파일에 동시 쓰기, ...
    }
    void OnFlowEnded(std::string_view obj, uniflow::StepAction action, ...) override {
        if (action == uniflow::StepAction::Fail) {
            // 실패한 flow는 Slack alert ...
        }
    }
};

int main() {
    uniflow::Runtime::Opts opts;
    opts.observer = std::make_unique<MyObserver>();
    uniflow::Runtime rt(std::move(opts));
    // ... 이 Runtime의 모든 모듈은 MyObserver가 받음
}
```

[examples/cnc_pickers/env_log_observer.h](examples/cnc_pickers/env_log_observer.h) 가 콘솔과 파일 *동시* 출력하는 실제 사용 예입니다.

**훅 일람**:
- `OnFlowStarted` - flow 시작
- `OnStepChanged` - step 전환 (가장 자주)
- `OnAsyncSubmitted` / `OnAsyncCompleted` - async 시작/끝
- `OnSlowCpuStep` - step 본문이 임계값보다 오래 펌프 잡고 있었을 때
- `OnSlowAsync` - async 작업이 임계값보다 오래 안 끝났을 때
- `OnSlowRound` - 한 라운드(pump 사이클)가 임계값보다 오래 (아래 "느린 사이클 추적")
- `OnStepThrew` - step에서 예외
- `OnFlowEnded` - flow 종료 (성공/실패 다)
- `OnPostSubmitted` / `OnPostExecuted` - `Post` / `PostAndWait` 제출/실행 (caller 동반, 챕터 9)
- `OnLinked` - `Link` 성립 (caller 동반, 챕터 9)

### 느린 사이클 추적 - 라운드 프로파일링

step/flow 통계는 "모듈 하나"를 보지만, 가끔 알고 싶은 건 *"펌프 한 바퀴(라운드)가 왜 갑자기 50ms 걸렸지?"* 입니다. 한 라운드 = post 비우기 + 모든 활성 모듈 1회 실행. 라운드 프로파일링이 이걸 봅니다.

```cpp
uniflow::Runtime::Opts o;
o.config.slow_round_threshold_ms = std::chrono::milliseconds(20); // 20ms 넘으면 알람
o.config.trace_rounds            = true;                          // step/post별 분해까지
uniflow::Runtime rt(std::move(o));
```

- **라운드 주기 통계** - `rt.GetRoundStats()` -> `{count, min_ms, max_ms, avg_ms, last_ms}` (일한 라운드만; idle 폴링 제외).
- **피크 리셋** - `max_ms` 가 피크. `rt.ResetRoundStats()` 로 초기화.
- **헤비 트레이스 on/off** - `rt.SetRoundTracing(true/false)`. 켜야 아래 `segments` 분해가 채워지고, 꺼져 있으면 라운드 길이(`busy_ms`)만 가볍게 받습니다. 런타임 토글 가능.
- **느린 사이클 알람** - 임계값 초과 시 `OnSlowRound(runtime_index, profile)`:

```cpp
void OnSlowRound(int rt_index, const uniflow::RoundProfile& p) override {
    // p.busy_ms        : 이 라운드 총 작업 시간
    // p.segments[i]    : { kind(Step/Post), obj, label, ms }  <- 범인 즉시 식별
    for (const auto& s : p.segments) { /* 가장 긴 ms 가 범인 */ }
}
```

기본 `ConsoleObserver` 가 찍어주는 모습:
```
[rt#0          ] [SLOW ROUND]  busy=52.10ms  segments=2
                 Step  Stage          OnProcess_WaitHwReady        48.30ms
                 Post  rt#0           net.cpp:88 OnPoll()           3.80ms
```

---

## 챕터 9. 여러 Runtime 사이 - Post와 Link

챕터 5에서 "Runtime 둘이면 펌프 둘, 그 사이 공유 자원은 락 필요" 라고 했습니다. 그런데 락을 거는 건 lock-free 라는 이 프레임워크의 전제를 버리는 일입니다. 대신 uniflow는 **접근을 한 펌프 스레드로 모으는** 두 가지 방법을 줍니다.

### Post - 콜백을 상대 펌프로 던지기

다른 runtime(또는 일반 스레드, 비-uniflow 코드)에서 어떤 runtime이 소유한 자원을 만지고 싶을 때, 직접 만지지 말고 콜백을 그 runtime에 `UF_POST` 합니다. 콜백은 그 runtime의 펌프 스레드에서 실행되니까 락이 필요 없습니다.

```cpp
uniflow::Runtime net_rt;
// ... net_rt 에 네트워크 모듈들 부착 ...

// main 스레드(또는 다른 runtime)에서:
UF_POST(net_rt, [] {
    // 이 람다는 net_rt 의 펌프 스레드에서 실행됨 - 락 불필요
    ConnectionTable::MarkAllStale();
});
```

이게 libuv의 `uv_async_send`, Qt의 `invokeMethod(..., Qt::QueuedConnection)` 과 같은 패턴입니다. 펌프는 매 라운드 맨 앞에서 쌓인 콜백을 비우고 실행합니다.

`UF_POST` 매크로는 호출 위치(파일명/줄/함수)를 자동으로 붙여서 observer 로깅에 넘깁니다. 매크로 없이 `net_rt.Post([]{...})` 로 직접 불러도 되지만, 그러면 로그의 caller 칸이 빕니다. 펌프를 넘나드는 콜백은 스택 트레이스에 안 잡히니, 어디서 던졌는지 로그에 남기려면 매크로 형태를 기본으로 쓰세요.

**규칙** - post된 콜백은 step/flow *밖*에서 도는 raw 콜백입니다. trace도 안 붙어요. 그러니 *짧고 논블로킹* 이어야 합니다. 펌프를 오래 잡으면 그 runtime 전체가 멈춥니다. 블로킹 일이 필요하면 콜백 안에서 `UF_START_FLOW` 로 flow를 켜세요.

### PostAndWait - 값을 받아와야 할 때

읽기가 필요하면 `UF_POST_WAIT`. 콜백이 펌프 스레드에서 실행되고, 호출한 스레드는 결과(`std::future`)를 기다립니다.

```cpp
std::future<int> f = UF_POST_WAIT(net_rt, [] {
    return ConnectionTable::Count();   // net_rt 펌프 위에서 안전하게 읽음
});
int count = f.get();                   // 호출 스레드는 여기서 블록
```

**절대 하면 안 되는 것** - 자기 자신을 구동하는 펌프 스레드에서 `UF_POST_WAIT` 부르기. 그 콜백을 실행할 펌프가 지금 결과를 기다리며 블록돼 있으니 영원히 안 풀립니다(데드락). step 본문에서 부르지 마세요 - assert가 잡아줍니다.

### Link - 두 펌프를 하나로 합치기

공유가 너무 잦아서 Post로는 번거로울 때, 아예 두 runtime을 한 펌프 스레드로 합칩니다. `driver.Link(other)` 하면:

- `other` 의 펌프 스레드는 멈추고
- `driver` 의 펌프가 `other` 의 모듈까지 매 라운드 돌립니다
- `other` 는 자기 observer / executor / config / 모듈 목록을 *그대로* 유지합니다 (펌프 스레드만 빌려줌)

```cpp
uniflow::Runtime rt;
uniflow::Runtime sub_rt;

SomeModule m{sub_rt};                  // 모듈은 sub_rt 소속
UF_LINK(rt, sub_rt);                   // 하지만 rt 의 펌프가 m 을 구동

UF_START_FLOW(m, OnSomething_Begin);   // rt 펌프 위에서 진행됨
```

합치고 나면 `rt` 의 모듈과 `sub_rt` 의 모듈이 한 스레드에서 직렬화되니, 둘 사이 공유 자원도 락이 필요 없습니다. 각 모듈의 slow 임계값 / observer / executor 는 자기 runtime 것을 그대로 쓰고, 펌프가 쉬는 주기(sleep cadence)만 driver 의 `Config` 를 따릅니다. `UF_LINK` 역시 호출 위치를 잡아 `OnLinked` observer 콜백에 넘깁니다.

**`Link` 는 단방향입니다** - 한 번 합치면 못 떼어냅니다. 합친 뒤 양쪽 flow가 서로 의존을 만들었을 수 있어서, 어느 모듈을 어느 펌프로 되돌려도 안전한지 알 수 없거든요. 그래서:

> **권장 기본값: Runtime 하나로 시작하세요.** 멀티 펌프는 "독립이 확실하고 + 병렬성이 진짜 필요할 때" 만 의식적으로 고르는 최적화입니다. "나중에 공유할 일 생기면?" 이 떠오른다는 것 자체가 독립이 확실하지 않다는 신호입니다.

### 로깅은 기본으로 남는다

세 동작 모두 observer로 흘러갑니다. 펌프를 넘나드는 제어 흐름은 디버깅이 까다로우니, 기본 `ConsoleObserver`가 caller 위치까지 찍어줍니다:

```
[rt#0          ] POST SUBMIT                  caller=net_worker.cpp:42 PollLoop()
[rt#0          ] POST RUN                     queue=0.67ms  caller=net_worker.cpp:42 PollLoop()
[rt#0          ] LINK                         rt#1 -> rt#0  caller=app.cpp:18 App::Start()
```

- `OnPostSubmitted` - post한 순간 (호출 스레드에서, caller 동반)
- `OnPostExecuted` - 펌프가 실제로 실행한 순간 (`queue=` 는 큐에서 기다린 시간)
- `OnLinked` - link가 성립한 순간

자체 observer를 꽂으면 이 셋을 override 해서 Prometheus / 사내 로그로 보낼 수 있습니다 (챕터 8).

### 언제 무엇을

- 공유가 **가끔** -> `UF_POST` / `UF_POST_WAIT` (국소적, 자원 하나만 한쪽 runtime 소유로)
- 공유가 **핫패스에서 빈번** -> `UF_LINK` (두 펌프를 합침)
- 셋 이상도 -> 한 driver 에 여러 개 `UF_LINK` 가능 (flat linking)

---

## 챕터 10. 가상 시간 - 배속, freeze, e-stop

모든 `UFTimer` 와 모든 `UF_STAY_UNTIL` 마감은 시간을 벽시계가 아니라 런타임의 *가상 시계* 에서 읽습니다. 기본은 실제 시간을 1:1 추종하지만, 배속하거나 멈출 수 있고 - 그 런타임의 모든 논리 타이머가 같이 움직입니다.

```cpp
rt.clock().SetScale(10.0);   // 논리 시간 10배속
rt.clock().SetScale(0.25);   // ... 또는 4배 느리게
rt.clock().Freeze();         // 논리 시간 정지
rt.clock().Resume();         // ... freeze 한 지점부터 이어감
```

이게 주는 두 가지:

- **시뮬레이션 재생.** `Passed(5s)` 대기와 `UF_STAY_UNTIL(3s, ...)` 타임아웃으로 가득한 흐름이 설정한 속도로 돕니다 - 하루짜리 라인 사이클을 테스트로 몇 초에 돌리거나, 느리게 늘려 관찰.
- **진짜로 올바른 일시정지.** e-stop/hold 중에, 라인이 10초 멈춰 있었다는 이유로 3초 "hw ready" 타임아웃이 터지면 곤란합니다. `Freeze()` 는 모든 논리 마감을 멈추고, `Resume()` 은 정확히 멈춘 지점부터 이어가서, 타임아웃이 벽시계가 아니라 *가동 시간* 기준이 됩니다.

**무엇이 스케일되고 무엇은 아닌가.** 가상 시계는 *논리* 대기에만 적용됩니다 - `UFTimer`(`Elapsed`/`Passed`/`HeldFor`)와 `UF_STAY_UNTIL`. `UF_ASYNC` / `UF_ASYNC_TIMEOUT` 데드라인이나 펌프 자체 낮잠은 일부러 건드리지 않습니다: 실제 네트워크 호출이 시뮬 배속 때문에 빨라지진 않으니까요. 그건 실제 벽시계 유지.

**바인딩.** `uniflow::UFTimer{rt.clock()}` 로 만든 타이머는 그 런타임 시계를 따릅니다. 맨 `uniflow::UFTimer{}` 는 실제 시간 - 배속/freeze 와 무관한 벽시계 측정이 굳이 필요할 때.

> 배속이나 freeze 는 *연속적* 입니다 - 변경 시점에 시계가 rebase 되어서 `SetScale`/`Freeze` 를 부른 순간 `Elapsed()` 가 점프하지 않습니다.

---

## 챕터 11. 외부에서 흐름 구동 - 이벤트 스레드와 `Wake`

실장비는 펌프가 아닌 스레드들이 찌릅니다: 메시지를 받는 소켓, 디바이스 드라이버 콜백, GUI 버튼. 그 이벤트로 흐름을 *즉시* 시작(또는 공급)하고 싶지, 다음 20ms 폴링까지 기다리고 싶진 않죠.

**다른 스레드에서 흐름을 시작하는 건 안전합니다.** `UF_START_FLOW`(과 `StartFlow`)는 내부에서 모듈 락을 잡으므로 어느 스레드에서나 불러도 됩니다:

```cpp
// 어떤 I/O 스레드에서:
void OnNetworkMessage(Message m) {
    UF_START_FLOW(App::inst().handler, OnHandle_Begin, std::move(m));
}
```

문제는: 이벤트가 도착할 때 펌프가 낮잠(`stay_sleep_ms`, 기본 20ms) 중일 수 있어서 첫 step 이 최대 20ms 늦게 돌 수 있다는 점. 그래서 `UF_START_FLOW` / `Post` 는 **펌프를 대신 깨워줍니다** - 낮잠에서 끌어내 갓 무장된 흐름이 바로 다음 라운드에 돕니다. 직접 부를 수도 있고요:

```cpp
rt.Wake();   // 다음 폴링 틱이 아니라 지금 낮잠에서 깨움
```

`Wake()` 를 직접 부를 때는? 흐름이 아니라 자기 채널로 모듈 상태를 바꿔놓고 지금 처리되게 하고 싶을 때. 외부에서 다른 런타임의 상태를 만지는 정석은 `UF_POST` 로 콜백을 던지는 것(챕터 9)이고 - `Post` 는 이미 펌프를 깨웁니다:

```cpp
UF_POST(rt, [&] { App::inst().handler.Enqueue(m); });   // 펌프에서 실행 + 깨움
```

> 외부 스레드에서 모듈 멤버를 **직접** 만지지 마세요 - 펌프와 레이스입니다. 흐름을 시작하거나 콜백을 `Post` 하세요; 둘 다 펌프 스레드에서 돌고, 둘 다 펌프를 깨웁니다. `Wake()` 는 그 둘이 깔고 있는 저수준 "낮잠 그만" 프리미티브일 뿐입니다.

하나 더: `UF_ASYNC` 잡이 끝날 때도 펌프를 깨우므로, 다음 step 이 결과를 폴링 간격만큼 늦지 않고 바로 캐치합니다.

---

## 챕터 12. 흐름 생명주기 제어 - idle, 대기, 정지

모듈은 *idle*(도는 흐름 없음) 이거나 *busy* 입니다. 이 생명주기를 다루는 호출 셋.

- **`IsIdle()`** - 모듈이 비었나? 오케스트레이터가 다른 모듈에 일을 주기 전에 확인합니다. `StartFlow` 자체도 이미 흐름이 돌고 있으면 `false` 를 돌려주고 아무것도 안 하므로, 가드가 자연스럽게 읽힙니다:

```cpp
if (worker.IsIdle())
    UF_START_FLOW(worker, OnWork_Begin, job);
```

- **`WaitUntilIdle()`** - 흐름이 끝날 때까지 *호출* 스레드를 블록. `main()` 이 종료 전에 작업이 빠지길 기다리는 방법입니다. 소유 스레드에서 부르고, step 안에서는 절대 부르지 마세요 (자기 펌프를 기다리며 블록하면 데드락):

```cpp
UF_START_FLOW(pipe, OnPipe_Fetch);
pipe.WaitUntilIdle();          // main 스레드가 흐름 끝까지 여기서 멈춤
```

**도는 흐름을 멈추는 건 협력적입니다.** step 도중에 흐름을 잡아채는 `Cancel()` 은 없습니다 - step 이 한창 쓰고 있는 상태를 부술 위험이 있으니까요. 대신 흐름은 step 이 *선택할 때* 끝납니다: stop 신호를 보고 `Done()` 또는 `Fail()` 을 반환. 오래 도는 모든 흐름이 쓰는 패턴입니다:

```cpp
StepResult OnRun_Tick() {
    if (GlobalEnv::Stopping()) {     // 어디서든 세팅하는 자기 플래그
        // ... 이 흐름이 쥔 것 정리 ...
        return Done();
    }
    // ... 정상 작업 ...
    return Stay();
}
```

체크가 그냥 step 이 플래그를 읽는 것이라, *어디서* 멈춰도 안전한지를 당신이 정합니다 - 모션 도중이 아니라 모션 사이에서. 모듈 여러 개를 질서있게 내리려면, 플래그를 세팅한 뒤 각각 `WaitUntilIdle()`:

```cpp
GlobalEnv::RequestStop();
for (auto* m : all_modules) m->WaitUntilIdle();
```

---

## 마지막: 다 합쳐서 - 오케스트레이션

여러 모듈이 *어느 순서로 무엇을 할지* 를 결정하는 모듈을 "오케스트레이터" 라고 부르겠습니다. 보통 flow 한 개 (`On*_Tick`)가 영원히 돌면서, 각 라운드에 `다른 모듈.IsIdle()`을 보고 적절히 `UF_START_FLOW`를 띄웁니다.

```cpp
class Scheduler : public uniflow::Uniflow<Scheduler> {
    UF_UNIFLOW_IMPLEMENT(Scheduler);
public:
    explicit Scheduler(uniflow::Runtime& rt)
        : uniflow::Uniflow<Scheduler>(rt) {}

    StepResult OnSched_Begin() { return UF_NEXT(OnSched_Tick); }

private:
    StepResult OnSched_Tick() {
        auto& worker = App::inst().worker;
        if (worker.IsIdle() && JobQueue::HasOne()) {
            UF_START_FLOW(worker, OnWorker_Begin, JobQueue::Pop());
        }
        if (GlobalEnv::Stop()) return Done();
        return Stay();
    }
};
```

이 패턴이 [cnc_pickers의 UF_Orchestrator](examples/cnc_pickers/uf_orchestrator.cpp), [queue_drain의 Sender](examples/queue_drain/uf_sender.cpp), [message_dispatch의 Professor/Friend](examples/message_dispatch/uf_professor.cpp) - 거의 모든 *실제* uniflow 모듈의 골격입니다.

---

## 그 다음

- [EXAMPLES.kr.md](EXAMPLES.kr.md) - 동작하는 예제 6개 둘러보기
- [DESIGN.md](../docs/DESIGN.md) - "왜 이렇게 디자인됐는지" 가 궁금해지면
- [uniflow.hpp](uniflow.hpp) - 헤더 자체. 핵심 클래스마다 주석 충실하게 달아둠

질문 / 버그 / 패치는 issue 또는 PR 환영입니다.
