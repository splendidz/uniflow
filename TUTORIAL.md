# uniflow 튜토리얼

> 🌐 언어: **한국어** · [English](TUTORIAL.en.md)

한 챕터에 한 개념씩. 챕터마다 *완전히 컴파일되고 도는* 코드 한 덩이를 줍니다. 챕터 8개 끝나면 [examples/](examples/) 안의 코드가 전부 자연스럽게 읽힙니다.

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

가장 작은 uniflow 모듈은 — step 함수 하나, Done() 한 번. 이게 끝입니다.

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
- `Hello h{rt};` — 모듈을 매다는 순간 펌프가 매 라운드 `h`를 방문하기 시작
- `UF_START_FLOW(h, OnHello_Begin)` — flow를 켬. 다음 라운드부터 step 호출
- step이 `Done()` 돌려줘서 모듈은 idle 상태로 복귀

**기억할 거 하나** — step 함수 안에서 `std::this_thread::sleep_for` 같은 거 *절대* 쓰지 마세요. 그 시간동안 펌프 전체가 멈춥니다. 기다리고 싶으면 `Stay()` (다음 챕터) 또는 `UF_ASYNC` (챕터 6).

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

**왜 굳이 step 함수로 자르나** — 콜백/async를 끼울 자리가 *바로 step 경계* 이기 때문입니다. 어떤 step에서 1초짜리 일을 시작했으면, 그 자리에서 `UF_ASYNC` 던지고 다음 step에서 결과 받습니다. 일이 끝날 때까지 펌프는 다른 모듈들을 돌리고 있어요.

**진입 step은 public** — 외부에서 `UF_START_FLOW`로 부르니까요. 중간 step은 private이 좋습니다 (이 모듈의 내부 흐름).

---

## 챕터 3. 폴링과 게이트 (Stay)

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

**`Stay()` 의 진짜 쓸모** — 외부 하드웨어 플래그 폴링, 다른 모듈의 상태 변화 기다리기, "조건 충족될 때까지 대기" 같은 거. *블로킹 일* (네트워크/디스크/긴 계산) 은 `Stay()` 가 아니라 `UF_ASYNC` 입니다.

---

## 챕터 4. Describe — 디버깅용 한 줄

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

`g_log`, `g_turn` 어디에도 mutex 없습니다 — 둘 다 같은 펌프 스레드 위에서만 만져지니까요. (예제 1 [shared_ostream](examples/shared_ostream/) 이 이 패턴의 완성판입니다.)

**경고** — Runtime을 *둘* 만들면 펌프가 둘이 됩니다. 그땐 두 펌프 사이의 공유 자원이 다시 멀티스레드 문제가 됩니다. 보통은 Runtime 하나로 충분하고, 정말 나눠야 할 때 둘 사이를 락 없이 잇는 방법(`Post` / `Link`)은 챕터 9에서 다룹니다.

---

## 챕터 6. 진짜 블로킹 작업 — UF_ASYNC

step 본문 안에서 5초짜리 HTTP 요청을 직접 호출하면? 그 5초 동안 펌프가 멈춥니다 → 다른 모듈도 다 멈춥니다. 절대 안 됩니다.

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

**왜 static만 되나** — 풀 스레드에서 도는 동안 모듈은 펌프 스레드 위에서 다른 step을 돌릴 수도 있는데, 거기서 멤버 변수를 만지면 race. 그래서 static을 강제하고, 필요한 데이터는 *인자로 복사해서* 풀로 넘깁니다. (`url_` 멤버를 인자로 한 번 복사함.)

결과는 `AsyncResult<T>()` 로 받습니다 — *연속 step 안에서만* 유효합니다. 한 step이 두 개를 동시에 띄울 수는 없어요(현재 in-flight는 한 번에 하나).

**관찰** — 콘솔에 자동으로 찍힙니다:
```
[Fetcher       ] (entry)                ASYNC SUBMIT  DoHttpGet
[Fetcher       ] (entry) -> OnFetch_Done ...
[Fetcher       ]                        ASYNC DONE    DoHttpGet  wait=120.4ms
[Fetcher       ] OnFetch_Done           got 1024 bytes  #01 elapsed=120.5ms
```

---

## 챕터 7. 타임아웃 / 에러 / 실패 처리

`UF_ASYNC_TIMEOUT(fn, dur, args...)` — 데드라인 붙은 async.

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
- `is_timeout()` true — 데드라인 넘김
- `failed()` true — 풀 스레드에서 예외 throw됨, `exception()` 에 ptr
- 둘 다 false — `value()` 가 유효

**step 본문에서 예외가 나면?** — 기본 동작은 *전체 종료* (`std::terminate`). 살리고 싶으면 모듈에 `CatchStepExceptions()`를 override 해서 `true` 돌려주세요:

```cpp
class SoftFail : public uniflow::Uniflow<SoftFail> {
    UF_UNIFLOW_IMPLEMENT(SoftFail);
public:
    bool CatchStepExceptions() const { return true; }
    // 이제 step에서 throw 나면 OnStepThrew → flow Fail() 로 끝남, 펌프 살아남음
};
```

---

## 챕터 8. observer 갈아끼우기

기본 observer (`ConsoleObserver`)는 step 전환, async 시작/끝, slow-step alarm 등을 콘솔에 예쁘게 적어줍니다. 더 정교한 게 필요하면 — 파일로 동시 출력, 로그 서버 전송, 메트릭 카운터 — `IUniflowObserver` 를 상속한 새 observer를 만들어서 Runtime에 꽂으면 됩니다.

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
- `OnFlowStarted` — flow 시작
- `OnStepChanged` — step 전환 (가장 자주)
- `OnAsyncSubmitted` / `OnAsyncCompleted` — async 시작/끝
- `OnSlowCpuStep` — step 본문이 임계값보다 오래 펌프 잡고 있었을 때
- `OnSlowAsync` — async 작업이 임계값보다 오래 안 끝났을 때
- `OnSlowRound` — 한 라운드(pump 사이클)가 임계값보다 오래 (아래 "느린 사이클 추적")
- `OnStepThrew` — step에서 예외
- `OnFlowEnded` — flow 종료 (성공·실패 다)
- `OnPostSubmitted` / `OnPostExecuted` — `Post` / `PostAndWait` 제출·실행 (caller 동반, 챕터 9)
- `OnLinked` — `Link` 성립 (caller 동반, 챕터 9)

### 느린 사이클 추적 — 라운드 프로파일링

step/flow 통계는 "모듈 하나"를 보지만, 가끔 알고 싶은 건 *"펌프 한 바퀴(라운드)가 왜 갑자기 50ms 걸렸지?"* 입니다. 한 라운드 = post 비우기 + 모든 활성 모듈 1회 실행. 라운드 프로파일링이 이걸 봅니다.

```cpp
uniflow::Runtime::Opts o;
o.config.slow_round_threshold_ms = std::chrono::milliseconds(20); // 20ms 넘으면 알람
o.config.trace_rounds            = true;                          // step/post별 분해까지
uniflow::Runtime rt(std::move(o));
```

- **라운드 주기 통계** — `rt.GetRoundStats()` → `{count, min_ms, max_ms, avg_ms, last_ms}` (일한 라운드만; idle 폴링 제외).
- **피크 리셋** — `max_ms` 가 피크. `rt.ResetRoundStats()` 로 초기화.
- **헤비 트레이스 on/off** — `rt.SetRoundTracing(true/false)`. 켜야 아래 `segments` 분해가 채워지고, 꺼져 있으면 라운드 길이(`busy_ms`)만 가볍게 받습니다. 런타임 토글 가능.
- **느린 사이클 알람** — 임계값 초과 시 `OnSlowRound(runtime_index, profile)`:

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

## 챕터 9. 여러 Runtime 사이 — Post와 Link

챕터 5에서 "Runtime 둘이면 펌프 둘, 그 사이 공유 자원은 락 필요" 라고 했습니다. 그런데 락을 거는 건 lock-free 라는 이 프레임워크의 전제를 버리는 일입니다. 대신 uniflow는 **접근을 한 펌프 스레드로 모으는** 두 가지 방법을 줍니다.

### Post — 콜백을 상대 펌프로 던지기

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

`UF_POST` 매크로는 호출 위치(파일명·줄·함수)를 자동으로 붙여서 observer 로깅에 넘깁니다. 매크로 없이 `net_rt.Post([]{...})` 로 직접 불러도 되지만, 그러면 로그의 caller 칸이 빕니다. 펌프를 넘나드는 콜백은 스택 트레이스에 안 잡히니, 어디서 던졌는지 로그에 남기려면 매크로 형태를 기본으로 쓰세요.

**규칙** — post된 콜백은 step/flow *밖*에서 도는 raw 콜백입니다. trace도 안 붙어요. 그러니 *짧고 논블로킹* 이어야 합니다. 펌프를 오래 잡으면 그 runtime 전체가 멈춥니다. 블로킹 일이 필요하면 콜백 안에서 `UF_START_FLOW` 로 flow를 켜세요.

### PostAndWait — 값을 받아와야 할 때

읽기가 필요하면 `UF_POST_WAIT`. 콜백이 펌프 스레드에서 실행되고, 호출한 스레드는 결과(`std::future`)를 기다립니다.

```cpp
std::future<int> f = UF_POST_WAIT(net_rt, [] {
    return ConnectionTable::Count();   // net_rt 펌프 위에서 안전하게 읽음
});
int count = f.get();                   // 호출 스레드는 여기서 블록
```

**절대 하면 안 되는 것** — 자기 자신을 구동하는 펌프 스레드에서 `UF_POST_WAIT` 부르기. 그 콜백을 실행할 펌프가 지금 결과를 기다리며 블록돼 있으니 영원히 안 풀립니다(데드락). step 본문에서 부르지 마세요 — assert가 잡아줍니다.

### Link — 두 펌프를 하나로 합치기

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

**`Link` 는 단방향입니다** — 한 번 합치면 못 떼어냅니다. 합친 뒤 양쪽 flow가 서로 의존을 만들었을 수 있어서, 어느 모듈을 어느 펌프로 되돌려도 안전한지 알 수 없거든요. 그래서:

> **권장 기본값: Runtime 하나로 시작하세요.** 멀티 펌프는 "독립이 확실하고 + 병렬성이 진짜 필요할 때" 만 의식적으로 고르는 최적화입니다. "나중에 공유할 일 생기면?" 이 떠오른다는 것 자체가 독립이 확실하지 않다는 신호입니다.

### 로깅은 기본으로 남는다

세 동작 모두 observer로 흘러갑니다. 펌프를 넘나드는 제어 흐름은 디버깅이 까다로우니, 기본 `ConsoleObserver`가 caller 위치까지 찍어줍니다:

```
[rt#0          ] POST SUBMIT                  caller=net_worker.cpp:42 PollLoop()
[rt#0          ] POST RUN                     queue=0.67ms  caller=net_worker.cpp:42 PollLoop()
[rt#0          ] LINK                         rt#1 -> rt#0  caller=app.cpp:18 App::Start()
```

- `OnPostSubmitted` — post한 순간 (호출 스레드에서, caller 동반)
- `OnPostExecuted` — 펌프가 실제로 실행한 순간 (`queue=` 는 큐에서 기다린 시간)
- `OnLinked` — link가 성립한 순간

자체 observer를 꽂으면 이 셋을 override 해서 Prometheus / 사내 로그로 보낼 수 있습니다 (챕터 8).

### 언제 무엇을

- 공유가 **가끔** → `UF_POST` / `UF_POST_WAIT` (국소적, 자원 하나만 한쪽 runtime 소유로)
- 공유가 **핫패스에서 빈번** → `UF_LINK` (두 펌프를 합침)
- 셋 이상도 → 한 driver 에 여러 개 `UF_LINK` 가능 (flat linking)

---

## 마지막: 다 합쳐서 — 오케스트레이션

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

이 패턴이 [cnc_pickers의 UF_Orchestrator](examples/cnc_pickers/uf_orchestrator.cpp), [queue_drain의 Sender](examples/queue_drain/uf_sender.cpp), [message_dispatch의 Professor/Friend](examples/message_dispatch/uf_professor.cpp) — 거의 모든 *실제* uniflow 모듈의 골격입니다.

---

## 그 다음

- [EXAMPLES.md](EXAMPLES.md) — 동작하는 예제 5개 둘러보기
- [DESIGN.md](DESIGN.md) — "왜 이렇게 디자인됐는지" 가 궁금해지면
- [uniflow.hpp](uniflow.hpp) — 헤더 자체. 핵심 클래스마다 주석 충실하게 달아둠

질문 / 버그 / 패치는 issue 또는 PR 환영입니다.
