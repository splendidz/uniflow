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

**경고** — Runtime을 *둘* 만들면 펌프가 둘이 됩니다. 그땐 모듈 사이 공유 자원에 락 필요. 보통은 Runtime 하나로 충분합니다.

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
- `OnStepThrew` — step에서 예외
- `OnFlowEnded` — flow 종료 (성공·실패 다)

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
