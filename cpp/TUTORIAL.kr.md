# uniflow 튜토리얼

> Language: [English](TUTORIAL.md) | **한국어**

한 챕터에 한 개념씩 다룬다. 챕터마다 현재 API(uniflow `1.0.0`) 기준으로 완전히 컴파일되고 동작하는 코드 한 덩이를 제공한다. 챕터들과 마지막 오케스트레이션까지 끝나면 [examples/](examples/) 안의 코드를 자연스럽게 읽을 수 있다.

> 처음이라면 [README](../README.kr.md)부터 보십시오. [왜 만들었는가](../README.kr.md#왜-만들었는가)는 이 프레임워크가 풀려는 문제 - flow를 C++로 짜는 흔한 세 방법과 각각이 실패하는 방식 - 와 거기서 따라 나온 사상을 정리한다. 이 튜토리얼은 그다음의 실습 단계로, 메커니즘을 하나씩 설명한다.

구성: 1-4장은 모델 자체 - 모듈, 태스크, 첫 step, step 체인, 폴링, 디버그 한 줄. 5-12장은 주변 장치(공유 상태, 블로킹 작업, 타임아웃, 옵저버, 멀티 런타임, 가상 시간, 수명). 13장은 다시 **태스크 구조** - 긴 flow를 컴파일러가 강제하는 여러 단위로 묶는, 모든 실제 flow의 구조적 골격으로 돌아온다.

1장에서 소개하고 이후 전반에 걸쳐 쓰는 두 개념:

- **모듈**은 `uniflow::Uniflow<Derived>` 를 상속한 클래스이다. 하나의 `Runtime`이 매달린 모든 모듈을 단일 펌프 스레드에서 구동한다.
- 모듈의 로직은 하나 이상의 **태스크**에 있다. 각 태스크는 `uniflow::Task<Flow>` 를 상속한 `struct` 이다. 태스크는 step들이 공유하는 상태와 step 함수 자체를 소유한다. step 한 개짜리 flow도 태스크이다. 이것이 고급 add-on이 아니라 기본 형태이므로, 첫 예제부터 태스크 기반 flow를 쓴다.

같은 예제 세트가 세 언어로 제공된다: 여기 C++, [python/examples](../python/examples)의 Python, [cs/examples](../cs/examples)의 C#. 이름이 세 언어에서 서로 대응된다.

각 챕터의 코드는 `tutorial/chapNN.cpp` 라고 가정한다. 빌드는:

```powershell
cl /std:c++17 /EHsc /I . tutorial\chap01.cpp /Fe:chap01.exe
```

또는 g++:
```bash
g++ -std=c++17 -O2 -pthread -I . tutorial/chap01.cpp -o chap01
```

> 모든 챕터는 `#include "uniflow.hpp"` 한 줄로 시작한다. 추가 의존성은 없다. 필요한 매크로는 `UF_FN(step)` 하나뿐이며, step 함수를 포인터와 라벨 쌍으로 만들어 `Next` / `StayTimeout` / `StayUntil` / `SubmitAsync` 에 넘긴다.

---

## 챕터 1. 한 step짜리 모듈

가장 작은 uniflow 모듈: 태스크 하나, step 하나, `Done()` 한 번. 태스크는 `Entry()` 를 override 해 첫 step을 가리키고, step은 의도(intent)를 반환한다.

```cpp
#include "uniflow.hpp"
#include <iostream>

class Flow_Hello : public uniflow::Uniflow<Flow_Hello>
{
public:
    explicit Flow_Hello(uniflow::Runtime& rt)
        : uniflow::Uniflow<Flow_Hello>(rt, "Flow_Hello")
    {
        AddTask(ctx_say_);
    }

    // The task is public so any thread (here, main) can launch it.
    struct Task_Say : uniflow::Task<Flow_Hello>
    {
        StepResult Entry() override { return Step1_Hello(); }

    private:
        StepResult Step1_Hello()
        {
            std::cout << "hello from a step\n";
            return Done();
        }
    } ctx_say_;
};

int main()
{
    uniflow::Runtime rt;
    Flow_Hello       hello{rt};
    hello.ctx_say_.StartFlow();   // launch the task
    hello.WaitUntilIdle();
}
```

콘솔 출력:
```
[Flow_Hello    ] FLOW START
hello from a step
[Flow_Hello    ] Step1_Hello             ...  #00 elapsed=0.05ms  tick x1 ...
[Flow_Hello    ] FLOW END  DONE  steps=#00 ...
```

**무슨 일이 일어났나**
- `Runtime rt;` - 펌프 스레드 1개를 띄운다.
- `Flow_Hello hello{rt};` - 모듈을 매다는 순간부터 펌프가 매 라운드 `hello`를 방문한다. 생성자가 `AddTask(ctx_say_)` 를 한 번 불러 태스크의 `flow()` 백포인터를 연결한다.
- `hello.ctx_say_.StartFlow()` - 태스크의 `Entry()` step에서 flow를 무장한다. step은 다음 라운드에 실행된다.
- `Entry()` 가 `Step1_Hello()` 를 반환하고, 그것이 출력한 뒤 `Done()` 을 반환하므로 모듈은 idle 로 복귀한다.

**조각 이름 정리.** *모듈*(`Flow_Hello`)은 런타임 위의 영속 객체이다. *태스크*(`Task_Say`)는 모듈이 돌릴 수 있는 한 단위 작업이다. *step*(`Step1_Hello`)은 그 태스크의 협력적 한 조각이다. step은 번호를 붙이고(`Step1_`, `Step2_`, ...) 자기 태스크의 private 멤버로 존재한다. public 인 것은 태스크뿐이며, 바깥에서 하는 일은 그것을 launch 하는 것뿐이기 때문이다.

**기억할 규칙 하나:** step 본문 안에서 `std::this_thread::sleep_for`(또는 어떤 블로킹 호출도) 절대 쓰지 않는다. 그 시간 동안 펌프 전체가 멈춘다. 기다리려면 `Stay()`(챕터 3) 또는 `SubmitAsync`(챕터 6)를 쓴다.

---

## 챕터 2. step 체인 (Next)

step 한 개로는 거의 충분하지 않다. 실제 태스크는 여러 step을 잇는다. `Next(UF_FN(다음_step))` 으로 같은 태스크의 형제 step으로 다음 펌프 라운드에 전진한다.

```cpp
#include "uniflow.hpp"
#include <iostream>

class Flow_Greet : public uniflow::Uniflow<Flow_Greet>
{
public:
    explicit Flow_Greet(uniflow::Runtime& rt)
        : uniflow::Uniflow<Flow_Greet>(rt, "Flow_Greet")
    {
        AddTask(ctx_greet_);
    }

    struct Task_Greet : uniflow::Task<Flow_Greet>
    {
        StepResult Entry() override { return Step1_Open(); }

    private:
        StepResult Step1_Open()
        {
            std::cout << "1. hi there\n";
            return Next(UF_FN(Step2_Middle));
        }
        StepResult Step2_Middle()
        {
            std::cout << "2. nice to see you\n";
            return Next(UF_FN(Step3_Close));
        }
        StepResult Step3_Close()
        {
            std::cout << "3. see you again\n";
            return Done();
        }
    } ctx_greet_;
};

int main()
{
    uniflow::Runtime rt;
    Flow_Greet       greet{rt};
    greet.ctx_greet_.StartFlow();
    greet.WaitUntilIdle();
}
```

**flow를 step으로 나누는 이유:** 콜백이나 async 결과를 끼우는 자리가 step 경계이기 때문이다. 어떤 step에서 1초짜리 일을 시작하면 그 자리에서 `SubmitAsync` 로 던지고 다음 step에서 결과를 받는다. 그 사이 펌프는 다른 모듈들을 계속 돌린다.

**`Next`는 태스크를 떠나지 않는다.** 이 태스크의 다른 step만 가리킬 수 있으며, 컴파일러가 이를 강제한다. `UF_FN(fn)` 이 `fn` 을 현재 태스크 타입에 대해 해석하기 때문이다. 다른 태스크로 넘어가는 것은 별개의 동작(`StartTask`, 챕터 13)이므로, flow가 한 단위에서 다음으로 소리 없이 흘러내릴 수 없다.

**체인을 따라 데이터 전달.** step은 파라미터를 가질 수 있다. 이름 뒤에 넘기면 다음 step으로 전달된다:

```cpp
return Next(UF_FN(Step2_Use), payload);   // Step2_Use(Payload p)
```

값은 다음-step thunk에 복사로 바인딩되어 현재 step 본문보다 오래 유지된다. 대표 용례는 `AsyncId` 를 그 결과를 읽는 step으로 전달하는 것이다(챕터 6).

---

## 챕터 3. 폴링 (Stay)

조건이 만족될 때까지 같은 step을 다시 돌리려면 `Stay()` 를 반환한다. 펌프는 `stay_sleep_ms`(기본 20ms) 만큼 쉰 뒤 같은 step을 다시 부른다.

```cpp
#include "uniflow.hpp"
#include <iostream>

class Flow_Counter : public uniflow::Uniflow<Flow_Counter>
{
public:
    explicit Flow_Counter(uniflow::Runtime& rt)
        : uniflow::Uniflow<Flow_Counter>(rt, "Flow_Counter")
    {
        AddTask(ctx_count_);
    }

    struct Task_Count : uniflow::Task<Flow_Counter>
    {
        // OnEnter runs once each time the task is entered, before the first
        // step - the place to (re-)arm a per-run timer.
        void OnEnter() override { t_.Restart(); }
        StepResult Entry() override { return Step1_Tick(); }

    private:
        uniflow::UFTimer t_;

        StepResult Step1_Tick()
        {
            using namespace std::chrono_literals;
            if (t_.Passed(5000ms))
            {
                std::cout << "5 seconds elapsed\n";
                return Done();
            }
            return Stay();   // not yet - come back next round
        }
    } ctx_count_;
};

int main()
{
    uniflow::Runtime rt;
    Flow_Counter     counter{rt};
    counter.ctx_count_.StartFlow();
    counter.WaitUntilIdle();
}
```

**`Stay()` 의 용도:** 하드웨어 플래그 폴링, 다른 모듈의 상태 변화 대기, 조건이 충족될 때까지의 대기. 실제 블로킹 작업(네트워크/디스크/긴 계산)은 `Stay()` 가 아니라 `SubmitAsync` 를 쓴다(챕터 6).

### `UFTimer` - step 안에서 시간 재기

단일 스레드 협력형 모델에서는 `sleep` 을 쓸 수 없다. 펌프 전체가 멈추기 때문이다. 그래서 "N ms 대기", "T 후 타임아웃", "D 동안 안정화" 같은 모든 시간 처리가 `Stay()` 라운드마다 폴링하는 타이머로 표현된다. 이 모델에서 시간은 이렇게 동작한다.

`uniflow::UFTimer` 는 읽기 3종을 가진 스톱워치이다:

```cpp
uniflow::UFTimer t;        // armed at construction; re-arm any time with t.Restart()

t.Elapsed();               // duration since it was armed
t.Passed(2000ms);          // bool: has 2s passed since arming?
t.HeldFor(cond, 50ms);     // bool: has 'cond' been continuously true for 50ms?
```

- **`Passed(d)`** - 무장 후 그 시간이 지났는가? 위 5초 대기가 이에 해당한다.

- **`HeldFor(cond, d)`** - 안정화(settling) / 디바운스. `cond` 가 `d` 동안 연속으로 참이어야 true이며, 한 번이라도 false면 카운트가 리셋된다. 안정되기 전에 튀는 하드웨어 플래그에 사용한다: `if (t.HeldFor(Hw::Ready(), 50ms)) ...`.

- **`Elapsed()`** - 원시 경과 시간. 페이싱이나 진행률에 사용한다: `double frac = to_ms(t.Elapsed()) / total_ms;`.

**보관 위치.** step을 넘나드는 타이머(step A에서 무장, step B에서 체크)는 step보다 오래 살아야 하므로 **태스크의 멤버**로 두고 `OnEnter()` 에서 재무장한다. 위 `t_` 가 이에 해당한다. 타이머가 태스크에 속하므로 모든 `Stay()` 재진입을 견디고, 다음 태스크 진입에서 리셋되며 수동 관리가 필요 없다.

**내장 per-step 타이머.** 모든 모듈은 step에서 `flow().StepTimer()` 로 닿는 내장 타이머도 가진다. 태스크 진입에서 리셋되는 per-task `TaskContext::Elapsed()` 와 달리, 이건 **모든 step 전환**(`Next`, `StayUntil` 타임아웃, 태스크 전환, flow 시작)에서 재무장되지만 `Stay` 에서는 안 되므로 멤버 선언 없이 현재 step 안의 경과를 잰다. 직접 만든 멤버 타이머에 같은 auto-reset을 주려면 평범한 `UFTimer` 대신 `flow().NewAutoTimer()` 로 만들면 된다 - 모듈이 등록된 모든 타이머를 step 전환마다 재무장한다. 셀프로 리셋하는 `UFTimer` 는 영향받지 않는다.

> **배속·freeze 되는 하나의 시계.** 타이머를 런타임에 바인딩하면 - `uniflow::UFTimer t{rt.clock()}` - 그 런타임의 가상 시계를 따른다. `rt.clock().SetScale(10)` 은 전체 흐름을 10배속 재생하고, `rt.clock().Freeze()` / `.Resume()` 은 모든 논리 타임아웃을 정지한다(예: e-stop 중 3초 타임아웃이 라인 멈춤 동안 터지지 않게). 그냥 `UFTimer{}` 는 실제 시간을 쓴다. async/IO 데드라인은 배속과 무관하게 항상 실제 시간이다. 챕터 10에서 다룬다.

조건을 폴링하되 **영영 안 오면 복구 step으로 빠지는** 경우는 챕터 7의 `StayTimeout` / `StayUntil` 을 참고한다.

---

## 챕터 4. Describe - 디버깅용 한 줄

각 step이 현재 동작을 observer/로그에 짧게 기록하면 디버깅이 쉬워진다. `Describe(...)` 가 그 줄을 만든다.

```cpp
StepResult Step3_WaitInPos()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    flow().axis_->Move(target_mm_);
    Describe("approaching target");      // <-- this line
    if (flow().axis_->InPosition())
    {
        return Next(UF_FN(Step4_Clamp));
    }
    return Stay();
}
```

콘솔 로그는 다음과 같이 나온다:
```
[Flow_Mover    ] Step3_WaitInPos        approaching target    #01 elapsed=42ms  ...
```

`Describe` 는 가변 인자를 받아 `<<` 으로 이어붙인다:
```cpp
Describe("parked at A-gap: stage=", ToString(stage_state),
         " partner_in_B=", partner_in_b);
```

step 전환 시 마지막으로 적힌 description이 한 번 출력되고 비워진다. step마다 새로 적어도 된다. (step은 `flow()` 로 모듈 자신의 상태에 접근한다. `flow()` 는 부모 모듈의 typed 참조이며 private 멤버에도 접근한다. 챕터 5에서 더 다룬다.)

---

## 챕터 5. 모듈 여럿이 한 Runtime을 공유

핵심 성질: **같은 Runtime에 매단 모듈들은 같은 펌프 스레드에서 돈다**. 따라서 모듈 사이 공유 자원에 락이 필요 없다.

```cpp
#include "uniflow.hpp"
#include <iostream>
#include <sstream>

namespace shared
{
std::ostringstream g_log;
int                g_turn = 0;
}

class Flow_Ping : public uniflow::Uniflow<Flow_Ping>
{
public:
    explicit Flow_Ping(uniflow::Runtime& rt)
        : uniflow::Uniflow<Flow_Ping>(rt, "Flow_Ping")
    {
        AddTask(ctx_ping_);
    }

    struct Task_Ping : uniflow::Task<Flow_Ping>
    {
        StepResult Entry() override { return Step1_Loop(); }

    private:
        int count_ = 5;

        StepResult Step1_Loop()
        {
            if (count_ <= 0)
            {
                return Done();
            }
            if (shared::g_turn != 0)
            {
                return Stay();   // wait my turn
            }
            shared::g_log << "ping ";
            shared::g_turn = 1;
            --count_;
            return Stay();
        }
    } ctx_ping_;
};

class Flow_Pong : public uniflow::Uniflow<Flow_Pong>
{
public:
    explicit Flow_Pong(uniflow::Runtime& rt)
        : uniflow::Uniflow<Flow_Pong>(rt, "Flow_Pong")
    {
        AddTask(ctx_pong_);
    }

    struct Task_Pong : uniflow::Task<Flow_Pong>
    {
        StepResult Entry() override { return Step1_Loop(); }

    private:
        int count_ = 5;

        StepResult Step1_Loop()
        {
            if (count_ <= 0)
            {
                return Done();
            }
            if (shared::g_turn != 1)
            {
                return Stay();
            }
            shared::g_log << "pong\n";
            shared::g_turn = 0;
            --count_;
            return Stay();
        }
    } ctx_pong_;
};

int main()
{
    uniflow::Runtime rt;
    Flow_Ping        ping{rt};
    Flow_Pong        pong{rt};
    ping.ctx_ping_.StartFlow();
    pong.ctx_pong_.StartFlow();
    ping.WaitUntilIdle();
    pong.WaitUntilIdle();
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

`g_log`, `g_turn` 어디에도 mutex가 없다. 둘 다 같은 펌프 스레드 위에서만 접근되기 때문이다.

**모듈 자신의 상태에 접근: `flow()`.** step은 모듈이 아니라 자기 태스크의 멤버이므로 `this` 는 태스크이다. 영속 하드웨어/피어 상태가 있는 소속 모듈은 `flow()` 로 접근한다. `flow()` 는 `AddTask` 가 연결한 typed 참조이다. 태스크가 모듈의 중첩 타입이므로 `flow()` 는 모듈의 private 멤버도 읽는다: `flow().axis_->Move(...)`, `flow().PartnerInZoneB()`. 형제 태스크의 상태도 같은 방식으로 접근한다: `flow().ctx_other_`.

**주의:** Runtime을 둘 만들면 펌프가 둘이 된다. 이 경우 서로 다른 Runtime의 모듈 간 공유 자원에 동기화가 필요하며, 둘을 락 없이 잇는 방법(`Post` / `Link`)은 챕터 9에서 다룬다. 보통은 Runtime 하나로 충분하다.

---

## 챕터 6. 진짜 블로킹 작업 - `SubmitAsync`

step 본문 안에서 5초짜리 HTTP 요청을 직접 호출하면 그 5초 동안 펌프가 멈추고, 다른 모듈도 모두 멈춘다. 이렇게 해서는 안 된다.

해법: `SubmitAsync(UF_FN(static_fn), timeout, args...)` 가 호출을 풀에 던지고 `AsyncId` 를 돌려준다. 그 id를 뒤 step으로 전달하고, 거기서 `AsyncResult<T>(id)` 로 결과를 폴링한다.

```cpp
#include "uniflow.hpp"
#include <iostream>
#include <string>

class Flow_Fetcher : public uniflow::Uniflow<Flow_Fetcher>
{
public:
    explicit Flow_Fetcher(uniflow::Runtime& rt)
        : uniflow::Uniflow<Flow_Fetcher>(rt, "Flow_Fetcher")
    {
        AddTask(ctx_fetch_);
    }

    struct Task_Fetch : uniflow::Task<Flow_Fetcher>
    {
        StepResult Entry() override { return Step1_Begin(); }

    private:
        StepResult Step1_Begin()
        {
            // No deadline -> Duration::max(). The id identifies this job.
            uniflow::AsyncId id =
                SubmitAsync(UF_FN(DoHttpGet), uniflow::Duration::max(),
                            std::string("http://example.com"));
            if (id == 0)
            {
                return Fail();   // submission rejected (in-flight cap)
            }
            return Next(UF_FN(Step2_Wait), id);   // carry the id forward
        }

        StepResult Step2_Wait(uniflow::AsyncId id)
        {
            auto r = AsyncResult<std::string>(id);
            if (r.pending())
            {
                return Stay();   // worker still in flight - poll again
            }
            if (!r.ok())
            {
                std::cout << "fetch failed\n";
                return Fail();
            }
            std::cout << "got " << r.return_value->size() << " bytes\n";
            return Done();
        }

        // The async target MUST be static (a free function works too). It runs
        // on a pool thread, so it cannot touch task / module state - pass
        // everything it needs by value.
        static std::string DoHttpGet(std::string url)
        {
            (void)url;
            // ... real HTTP call happens here, on a pool thread ...
            return "<html>...</html>";
        }
    } ctx_fetch_;
};

int main()
{
    uniflow::Runtime rt;
    Flow_Fetcher     fetcher{rt};
    fetcher.ctx_fetch_.StartFlow();
    fetcher.WaitUntilIdle();
}
```

**static만 되는 이유:** 풀 스레드에서 도는 동안 모듈은 펌프 스레드 위에서 다른 step을 돌릴 수 있는데, 양쪽에서 멤버에 접근하면 race가 발생한다. 그래서 static을 강제하고, 필요한 데이터는 인자로 복사해서 넘긴다(URL 문자열이 인자로 복사된다).

**결과는 폴링으로 읽는다.** `AsyncResult<T>(id)` 는 `AsyncOutcome<T>` 를 값으로 돌려준다. 매 라운드 확인한다:
- `r.pending()` - 아직 진행 중; `Stay()` 하고 다시 폴링.
- `r.ok()` - 워커가 반환함; `*r.return_value`(`std::optional<T>`)에 값.
- `r.failed()` / `r.is_timeout()` - 워커가 throw 했거나 데드라인을 놓침.
- `r.found()` - 잘못됐거나/clear됐거나/`0` id 일 때만 false.

여러 잡이 동시에 in-flight일 수 있으며, 각자 id를 가진다. `AnyAsyncPending()` 은 미해결 잡이 하나라도 있으면 true; `JoinAllAsync(UF_FN(then))` 은 전부 해결될 때까지 `Stay()` 하다 전진; `ClearAsync()` 는 모든 슬롯을 버린다.

**관찰:** 콘솔에 자동으로 출력된다:
```
[Flow_Fetcher  ] Step1_Begin             ASYNC SUBMIT  DoHttpGet
[Flow_Fetcher  ] Step1_Begin -> Step2_Wait ...
[Flow_Fetcher  ]                        ASYNC DONE    DoHttpGet  wait=120.4ms
[Flow_Fetcher  ] Step2_Wait              got 1024 bytes  #01 elapsed=120.5ms
```

---

## 챕터 7. 타임아웃 / 에러 / 실패 처리

타임아웃 두 종류, 역할 둘. **async 잡**이 데드라인을 넘길 수 있고, **폴링 step**이 영영 안 오는 플래그를 무한히 기다릴 수 있다. uniflow는 각각에 도구 하나씩을 둔다.

### async 데드라인 - `SubmitAsync` 타임아웃

`SubmitAsync` 의 두 번째 인자가 데드라인이다. `Duration::max()` 대신 실제 `Duration` 을 넘기면, 그것을 놓친 워커는 `TimedOut` 으로 해결된다:

```cpp
using namespace std::chrono_literals;

StepResult Step1_Query()
{
    uniflow::AsyncId id = SubmitAsync(UF_FN(SlowApi), 2000ms, query_);   // 2s or bust
    if (id == 0)
    {
        return Fail();
    }
    return Next(UF_FN(Step2_After), id);
}

StepResult Step2_After(uniflow::AsyncId id)
{
    auto r = AsyncResult<Response>(id);
    if (r.pending())
    {
        return Stay();
    }
    if (r.is_timeout())
    {
        Describe("API did not respond in 2s");
        return Fail();
    }
    if (r.failed())
    {
        Describe("API worker threw");
        return Fail();
    }
    return Next(UF_FN(Step3_Use), *r.return_value);
}
```

`AsyncOutcome<T>` 가 슬롯을 분류한다 - `is_timeout()`, `failed()`, `ok()`, `pending()` - 그래서 없는 결과를 역참조할 일이 없다. (`.value()` 나 `.exception()` 은 없다. 값은 `return_value` 에 있고 `ok()` 일 때만 engaged된다.)

### step 데드라인 - `StayTimeout`, step 단위 `catch`

잡을 기다리는 것이 아닐 때가 많다. 하드웨어에 명령을 내려놓고 "완료" 플래그·센서·피어 모듈 상태를 `Stay()` 로 폴링하는 경우이다. 그것이 영영 안 오면 - 축이 끼이거나, 엔코더가 끊기거나, 밸브가 멈추면 - 맨 `Stay()` 루프는 무한히 폴링하고, 라인은 에러도 없이 조용히 멈춰 선다. 실제 장비에서 이는 최악의 결과 중 하나이다.

`StayTimeout(dur, UF_FN(fn))` 은 **마감이 달린 `Stay()`** 이다: 이 step을 계속 폴링하되, step에 진입한 시점부터 `dur` 이 지나면 step `fn` 으로 빠진다. `fn` 이 곧 `catch` 이며, 정해진 복구 경로로의 보장된 탈출구이다. (성공 경로는 여전히 본문이 소유한다 - 본문이 직접 `Next`/`Done` 을 반환하고, 마감은 대기가 끝내 안 풀릴 때의 탈출만 보장한다.)

전체 패턴은 명령, 마감 걸린 대기, 복구이다:

```cpp
using namespace std::chrono_literals;

// Move an axis to a target, then wait for it to report InPosition.
StepResult Step1_Command()
{
    flow().axis_->Move(target_mm_);              // fire the command (non-blocking)
    Describe("moving to ", target_mm_, " mm");
    return Next(UF_FN(Step2_WaitInPos));
}

StepResult Step2_WaitInPos()
{
    if (flow().axis_->InPosition())              // the happy path
    {
        return Next(UF_FN(Step3_Clamp));
    }
    // still moving - keep polling, but give up if it stalls past 2s
    return StayTimeout(2000ms, UF_FN(Step_Stalled));
}

// Reached ONLY if InPosition never became true within 2s of entering the
// wait step. The flow cannot hang - it always lands somewhere defined.
StepResult Step_Stalled()
{
    flow().axis_->Abort();                       // stop the motion
    Describe("axis stalled before reaching target");
    return Fail();
}
```

`StayTimeout` 없이 `Step2_WaitInPos` 가 맨 `Stay()` 를 반환했다면, 누군가 라인이 죽은 것을 알아챌 때까지 무한히 돈다. 이것을 쓰면 이동이 끝나지 않을 경우 `Step_Stalled` 에 반드시 도달한다. 흐름은 늘 정의된 상태로 전진한다.

**복구 step도 일반 step이므로 어디로든 라우팅할 수 있다.** 흔한 형태는 재시도 후 포기이다:

```cpp
StepResult Step2_WaitInPos()
{
    if (flow().axis_->InPosition())
    {
        return Next(UF_FN(Step3_Clamp));
    }
    return StayTimeout(2000ms, UF_FN(Step_Retry));
}

StepResult Step_Retry()
{
    if (++attempts_ >= 3)                          // out of retries
    {
        Describe("axis failed to reach target after 3 tries");
        return Fail();
    }
    flow().axis_->Abort();
    Describe("retry ", attempts_, "/3");
    return Next(UF_FN(Step1_Command));             // re-issue -> re-enters the
}                                                  // wait, restarting the 2s window
```

(`attempts_` 는 태스크의 멤버로, `OnEnter()` 에서 리셋한다.) `Step1_Command` -> `Step2_WaitInPos` 재진입은 새 step 진입이므로 시도마다 2s 마감이 새로 시작되며, 수동 타이머 관리가 필요 없다.

알아둘 세 가지:

- **마감은 `StayTimeout` 호출 시점이 아니라 step 진입 기준이다.** 매 폴링 틱마다 반환하지만 시계는 틱마다 리셋되지 않는다. 2s 폴링하는 step은 정확히 2s에 타임아웃된다.
- **논리 시간이다.** 마감은 런타임 시계(챕터 3의 `rt.clock()`) 위에서 돌므로 `Freeze()` 하면 멈추고(e-stop 중 2s 타임아웃이 터지지 않음) `SetScale` 로 스케일된다. `SubmitAsync` 데드라인은 실제 시간을 유지한다.
- **타임아웃 둘, 역할 둘.** `SubmitAsync` 타임아웃은 "이 잡이 T 안에 끝나야 함"을 뜻한다(폴링할 때 `is_timeout()` 을 읽는다). `StayTimeout` 은 "이 step이 T 안에 진전돼야 함"을 뜻한다(복구 step으로 빠진다). 앞은 async 작업에, 뒤는 폴링 조건에 쓴다.

챕터 3의 `HeldFor` 와도 짝이 된다. 플래그가 안정되길 요구하되, 끝내 정착하지 않으면 빠진다:

```cpp
StepResult Step2_WaitReady()
{
    if (settle_.HeldFor(flow().hw_ready_->IsReady(), 50ms))   // ready AND settled 50ms
    {
        return Next(UF_FN(Step3_Done));
    }
    return StayTimeout(3000ms, UF_FN(Step_Timeout));            // never settled -> recover
}
// settle_ is a UFTimer member of the task, re-armed in OnEnter().
```

이 settle 대기 짝은 흔해서 **`StayUntil`** 이 한 호출로 접어준다: 대기 조건 + settle 시간 + 두 타깃. `condition` 을 매 라운드 폴링해 `settle` 동안 유지되면 success 타깃으로, 그 전에 `timeout` 이 지나면 타임아웃 타깃으로 간다. 인자 순서는 `condition, settle, success, timeout, timeout_step` (Python / C# 포트와 동일). 위 `Step2_WaitReady` 전체가 한 줄이 된다:

```cpp
StepResult Step2_WaitReady()
{
    return StayUntil([this] { return flow().hw_ready_->IsReady(); }, 50ms,
                     UF_FN(Step3_Done), 3000ms, UF_FN(Step_Timeout));
}
```

여기 settle 구간은 프레임워크가 추적하며(멤버 타이머 불필요), 아래 내장 타이머처럼 step 진입마다 리셋된다.

**step 본문에서 예외가 나면:** 기본 동작은 전체 종료(`std::terminate`)이다. 계속 진행하려면 모듈에 `CatchStepExceptions()` 를 override 해 `true` 를 돌려준다:

```cpp
class Flow_SoftFail : public uniflow::Uniflow<Flow_SoftFail>
{
public:
    explicit Flow_SoftFail(uniflow::Runtime& rt)
        : uniflow::Uniflow<Flow_SoftFail>(rt, "Flow_SoftFail") {}

    // Now an exception in a step fires OnStepThrew, the flow ends with
    // Fail(), and the pump keeps running other modules.
    bool CatchStepExceptions() const { return true; }
};
```

---

## 챕터 8. observer 갈아끼우기

기본 observer(`ConsoleObserver`)는 step 전환, async 시작/끝, slow-step alarm을 콘솔에 형식화해 출력한다. 추가 동작이 필요하면 - 파일 동시 출력, 로그 서버 전송, 메트릭 - `IUniflowObserver` 를 상속해 Runtime에 설치한다.

```cpp
class MyObserver : public uniflow::IUniflowObserver
{
public:
    void OnStepChanged(std::string_view obj,
                       std::string_view prev_step, std::string_view next_step,
                       std::string_view description,
                       int step_ordinal, double elapsed_ms,
                       const uniflow::TickStats& step_ticks) override
    {
        // your own format, file mirror, ...
    }

    void OnFlowEnded(std::string_view obj, uniflow::StepAction terminal_action,
                     int final_step_ordinal,
                     const std::vector<uniflow::TraceEntry>& trace,
                     double wall_ms, double total_step_ms, double total_async_ms,
                     const uniflow::FlowTickSummary& flow_ticks,
                     const uniflow::FlowStats& stats,
                     uniflow::FlowOrigin origin) override
    {
        if (terminal_action == uniflow::StepAction::Fail)
        {
            // page on Slack ...
        }
    }
};

int main()
{
    uniflow::Runtime::Opts opts;
    opts.observer = std::make_unique<MyObserver>();
    uniflow::Runtime rt{std::move(opts)};
    // ... every module on this Runtime reports through MyObserver
}
```

**빈** 서브클래스는 침묵 observer이다. 모든 훅이 기본 no-op이므로, 화면을 직접 소유하는 콘솔 앱이 프레임워크 출력을 완전히 끌 수 있다:

```cpp
struct SilentObserver : uniflow::IUniflowObserver {};
```

[examples/pick_and_place/env_log_observer.h](examples/pick_and_place/env_log_observer.h) 가 콘솔·파일 동시 출력의 실제 예이고, [simulator](examples/simulator/) 가 위 침묵 패턴을 쓴다.

**훅 일람**(전부 선택):
- `OnFlowStarted` - 태스크 무장됨.
- `OnStepChanged` - step 전환(가장 자주).
- `OnAsyncSubmitted` / `OnAsyncCompleted` - async 시작/끝.
- `OnSlowCpuStep` - step이 임계값보다 오래 펌프를 잡음.
- `OnSlowAsync` - async 잡이 임계값보다 오래 in-flight.
- `OnAsyncAbandoned` / `OnAsyncHighWater` - 워커가 flow보다 오래 삶 / in-flight 상한 도달.
- `OnStepThrew` - step에서 예외.
- `OnFlowEnded` - flow 종료(성공/실패 다).
- `OnPostSubmitted` / `OnPostExecuted` / `OnLinked` - 크로스 런타임 트래픽(챕터 9).
- `OnSlowRound` - 펌프 한 라운드 초과(아래).

### 느린 사이클 추적 - 라운드 프로파일링

step/flow 통계는 모듈 하나를 보지만, 때로 알아야 하는 것은 펌프 한 라운드가 왜 50ms 걸렸는지이다. 한 라운드는 post를 비우고 모든 활성 모듈을 1회 실행한다. 라운드 프로파일링이 이를 측정한다.

```cpp
uniflow::Runtime::Opts o;
o.config.slow_round_threshold_ms = std::chrono::milliseconds(20); // alarm past 20ms
o.config.trace_rounds            = true;                          // + per-step/post breakdown
uniflow::Runtime rt{std::move(o)};
```

- **라운드 주기 통계** - `rt.GetRoundStats()` -> `{count, min_ms, max_ms, avg_ms, last_ms}` (일한 라운드만; idle 폴링 제외).
- **피크 리셋** - `max_ms` 가 피크이다. `rt.ResetRoundStats()` 로 초기화한다.
- **헤비 트레이스 on/off** - `rt.SetRoundTracing(true/false)`. 켜야 아래 분해가 채워지고, 꺼져도 라운드 길이는 낮은 비용으로 얻는다. 런타임에 토글할 수 있다.
- **느린 사이클 알람** - 임계값 초과 시 `OnSlowRound(runtime_index, profile)`:

```cpp
void OnSlowRound(int rt_index, const uniflow::RoundProfile& p) override
{
    // p.busy_ms     : total work time this round
    // p.segments[i] : { kind(Step/Post), obj, label, ms }  <- names the culprit
    for (const auto& s : p.segments) { /* the longest ms is the culprit */ }
}
```

기본 `ConsoleObserver` 의 출력:
```
[rt#0          ] [SLOW ROUND]  busy=52.10ms  segments=2
                 Step  Flow_Stage     Step1_Process                48.30ms
                 Post  rt#0           net.cpp:88 OnPoll()           3.80ms
```

---

## 챕터 9. 여러 Runtime 사이 - Post와 Link

챕터 5에서 Runtime 둘이면 펌프 둘이고 그 사이 공유 자원에 락이 필요하다고 했다. 그런데 락을 거는 것은 lock-free 라는 이 프레임워크의 전제를 버리는 일이다. 대신 uniflow는 **접근을 한 펌프 스레드로 모으는** 두 가지 방법을 제공한다.

### Post - 콜백을 상대 펌프로 던지기

다른 runtime(또는 일반 스레드, 비-uniflow 코드)에서 어떤 runtime이 소유한 자원에 접근하려 할 때, 직접 접근하지 말고 콜백을 그 runtime에 `Post` 한다. 콜백은 그 runtime의 펌프 스레드에서 실행되므로 락이 필요 없다.

```cpp
uniflow::Runtime net_rt;
// ... network modules attached to net_rt ...

// from the main thread (or another runtime):
net_rt.Post([] {
    // this lambda runs on net_rt's pump thread - no lock
    ConnectionTable::MarkAllStale();
});
```

이것은 libuv의 `uv_async_send`, Qt의 `invokeMethod(..., Qt::QueuedConnection)` 과 같은 패턴이다. 펌프는 매 라운드 맨 앞에서 쌓인 콜백을 비우고 실행하며, 스스로를 깨워 이번 라운드에 처리한다.

> 더 풍부한 로깅을 원하면 `PostAt(caller, fn)` 이 호출 위치(파일/줄/함수)를 붙여 `OnPostSubmitted` / `OnPostExecuted` 가 출처를 보고하게 한다. 맨 `Post` 는 빈 call site로 post한다.

**규칙:** post된 콜백은 step/flow 밖에서 도는 raw 콜백이다(trace 없음). 그러므로 짧고 논블로킹이어야 한다. 펌프를 오래 잡으면 그 runtime 전체가 멈춘다. 블로킹 작업이 필요하면 콜백 안에서 `ctx.StartFlow()` 로 flow를 시작한다.

### PostAndWait - 값을 받아와야 할 때

값을 받아와야 하면 `PostAndWait` 를 쓴다. 콜백이 펌프 스레드에서 실행되고, 호출한 스레드는 결과(`std::future`)를 기다린다.

```cpp
std::future<int> f = net_rt.PostAndWait([] {
    return ConnectionTable::Count();   // read safely on net_rt's pump
});
int count = f.get();                   // calling thread blocks here
```

**절대 하면 안 되는 것:** 대상 runtime을 구동하는 펌프 스레드에서 `PostAndWait` 를 부르는 것. 그 콜백을 실행할 펌프가 결과를 기다리며 블록되어 있으므로 영원히 풀리지 않는다(데드락). step 본문에서 부르지 않는다. assert가 이를 잡는다.

### Link - 두 펌프를 하나로 합치기

공유가 잦아 Post가 번거로울 때, 두 runtime을 한 펌프 스레드로 합친다. `driver.Link(other)` 하면:

- `other` 의 펌프 스레드는 멈추고
- `driver` 의 펌프가 `other` 의 모듈까지 매 라운드 돌리며
- `other` 는 자기 observer / executor / config / 모듈 목록을 그대로 유지한다(펌프 스레드만 빌려준다)

```cpp
uniflow::Runtime rt;
uniflow::Runtime sub_rt;

Flow_Something m{sub_rt};   // module belongs to sub_rt
rt.Link(sub_rt);           // but rt's pump drives m

m.ctx_run_.StartFlow();    // runs on rt's pump
```

합치고 나면 `rt` 의 모듈과 `sub_rt` 의 모듈이 한 스레드에서 직렬화되므로, 둘 사이 공유 자원도 락이 필요 없다. 각 모듈의 slow 임계값 / observer / executor 는 자기 것을 그대로 쓰고, 펌프 쉬는 주기만 driver 의 `Config` 를 따른다. `LinkAt` 은 호출 위치를 잡아 `OnLinked` 에 넘긴다.

**`Link` 는 단방향이다.** 한 번 합치면 떼어낼 수 없다. 합친 뒤 양쪽 flow가 서로 의존을 형성했을 수 있어, 어느 모듈을 어느 펌프로 되돌려도 안전한지 판단할 수 없기 때문이다. 그래서:

> **권장 기본값: Runtime 하나로 시작한다.** 멀티 펌프는 독립이 확실하고 병렬성이 실제로 필요할 때만 의식적으로 선택하는 최적화이다. "나중에 공유할 일이 생기면?" 이라는 질문이 떠오른다면, 이는 독립이 확실하지 않다는 신호이다.

### 로깅은 기본으로 남는다

세 동작 모두 observer로 흘러간다. 펌프를 넘나드는 제어 흐름은 디버깅이 까다로우므로, 기본 `ConsoleObserver` 가 caller 위치까지 출력한다:

```
[rt#0          ] POST SUBMIT                  caller=net_worker.cpp:42 PollLoop()
[rt#0          ] POST RUN                     queue=0.67ms  caller=net_worker.cpp:42 PollLoop()
[rt#0          ] LINK                         rt#1 -> rt#0  caller=app.cpp:18 App::Start()
```

### 언제 무엇을

- 공유가 **가끔** -> `Post` / `PostAndWait` (국소적, 자원 하나만 한쪽 runtime 소유).
- 공유가 **핫패스에서 빈번** -> `Link` (두 펌프 합침).
- 셋 이상도 -> 한 driver에 여러 개 `Link` 가능(flat linking).

---

## 챕터 10. 가상 시간 - 배속, freeze, e-stop

모든 `UFTimer` 와 모든 `StayTimeout` / `StayUntil` 마감은 시간을 벽시계가 아니라 런타임의 가상 시계에서 읽는다. 기본은 실제 시간을 1:1 추종하지만, 배속하거나 멈출 수 있으며, 그 런타임의 모든 논리 타이머가 같이 움직인다.

```cpp
rt.clock().SetScale(10.0);   // logical time runs 10x faster
rt.clock().SetScale(0.25);   // ... or 4x slower
rt.clock().Freeze();         // logical time stops
rt.clock().Resume();         // ... and continues from where it froze
```

이것이 제공하는 두 가지:

- **시뮬레이션 재생.** `Passed(5000ms)` 대기와 `StayTimeout(3000ms, ...)` 타임아웃을 가진 흐름이 설정한 속도로 돈다. 하루짜리 라인 사이클을 테스트로 몇 초에 돌리거나, 느리게 늘려 관찰한다. [simulator](examples/simulator/) 예제가 이 경우이다: 다섯 러너와 렌더러가 한 시계를 공유하고, 한 번의 `SetScale` / `Freeze` 가 전체를 한꺼번에 스케일·정지한다.
- **올바른 일시정지.** e-stop/hold 중에, 라인이 10초 멈춰 있었다는 이유로 3초 "hw ready" 타임아웃이 터지면 안 된다. `Freeze()` 는 모든 논리 마감을 멈추고, `Resume()` 은 멈춘 지점부터 이어가며, 타임아웃이 벽시계가 아니라 가동 시간 기준이 된다.

**무엇이 스케일되고 무엇은 아닌가.** 가상 시계는 논리 대기에만 적용된다 - `UFTimer`(`Elapsed`/`Passed`/`HeldFor`)와 `StayTimeout` / `StayUntil`. `SubmitAsync` 데드라인이나 펌프 자체 낮잠은 일부러 건드리지 않는다: 실제 네트워크 호출이 시뮬 배속 때문에 빨라지지 않기 때문이다. 그것은 실제 벽시계를 유지한다.

**바인딩.** `uniflow::UFTimer{rt.clock()}` 로 만든 타이머는 그 런타임 시계를 따른다. 맨 `uniflow::UFTimer{}` 는 실제 시간을 쓰며, 배속/freeze 와 무관한 벽시계 측정이 필요할 때 사용한다. 가상 시간을 직접 읽으려면 `rt.clock().Now()` 를 쓴다.

> 배속이나 freeze 는 연속적이다. 변경 시점에 시계가 rebase 되어 `SetScale` / `Freeze` 를 부른 순간 `Elapsed()` 가 점프하지 않는다.

---

## 챕터 11. 외부에서 흐름 구동 - 이벤트 스레드와 `Wake`

실장비는 펌프가 아닌 스레드들로부터 이벤트를 받는다: 메시지를 받는 소켓, 디바이스 드라이버 콜백, GUI 버튼. 그 이벤트로 흐름을 즉시 시작(또는 공급)해야 하며, 다음 20ms 폴링까지 기다려서는 안 된다.

**다른 스레드에서 태스크를 launch 하는 것은 안전하다.** `StartFlow` / `StartTask` 는 내부에서 모듈 락을 잡으므로 어느 스레드에서나 부를 수 있다:

```cpp
// on some I/O thread:
void OnNetworkMessage()
{
    App::inst().handler.ctx_handle_.StartFlow();
}
```

고려할 점: 이벤트가 도착할 때 펌프가 낮잠(`stay_sleep_ms`, 기본 20ms) 중일 수 있어 첫 step이 최대 20ms 늦게 돌 수 있다. 그래서 `StartFlow` / `Post` 는 **펌프를 깨운다** - 낮잠에서 끌어내 갓 무장된 흐름이 다음 라운드에 돈다. 직접 부를 수도 있다:

```cpp
rt.Wake();   // pump out of its nap now, not at the next poll tick
```

`Wake()` 를 직접 부르는 경우는 흐름이 아니라 별도 채널로 모듈 상태를 바꾼 뒤 지금 처리되게 해야 할 때이다. 외부에서 다른 런타임 상태에 접근하는 정석은 `Post` 로 콜백을 보내는 것(챕터 9)이며, `Post` 는 이미 펌프를 깨운다:

```cpp
net_rt.Post([&] { App::inst().handler.Enqueue(m); });   // runs on the pump, wakes it
```

> 외부 스레드에서 모듈 멤버에 **직접** 접근하지 않는다. 펌프와 레이스가 발생한다. 태스크를 launch 하거나 콜백을 `Post` 한다. 둘 다 펌프 스레드에서 돌고 둘 다 펌프를 깨운다. `Wake()` 는 그 둘이 기반으로 삼는 저수준 wake 프리미티브이다.

이는 `SubmitAsync` 잡이 끝날 때도 적용된다: 펌프를 깨우므로 폴링하는 step이 결과를 폴링 간격만큼 늦지 않고 바로 읽는다.

---

## 챕터 12. 흐름 생명주기 제어 - idle, 대기, 정지

모듈은 *idle*(도는 태스크 없음)이거나 *busy*(태스크 하나 도는 중; 모듈은 한 번에 한 태스크) 이다. 이 생명주기를 다루는 호출 셋.

- **`IsIdle()`** - 모듈이 비었는가? 오케스트레이터가 피어에 태스크를 launch 하기 전 확인한다. `StartFlow` 자체도 이미 태스크가 돌고 있으면 `StartResult::Busy` 를 돌려주고 아무것도 하지 않으므로, 가드가 명확하게 읽힌다:

```cpp
if (worker.IsIdle())
{
    worker.ctx_run_.StartFlow();
}
```

- **`WaitUntilIdle()`** - 도는 태스크가 끝날 때까지 호출 스레드를 블록한다. `main()` 이 종료 전 작업이 빠지길 기다리는 방법이다. 소유 스레드에서 부르고, step 안에서는 절대 부르지 않는다(자기 펌프를 기다리며 블록하면 데드락):

```cpp
pipe.ctx_fetch_.StartFlow();
pipe.WaitUntilIdle();          // main thread parks here until the task ends
```

**도는 흐름을 멈추는 것은 협력적이다.** step 도중에 태스크를 잡아채는 `Cancel()` 은 없다. step이 사용 중인 상태를 부술 위험이 있기 때문이다. 대신 흐름은 step이 선택할 때 끝난다: stop 신호를 보고 `Done()` 또는 `Fail()` 을 반환한다. 오래 도는 모든 태스크가 쓰는 패턴이다:

```cpp
StepResult Step1_Tick()
{
    if (GlobalEnv::Stop())           // your own flag, set from anywhere
    {
        // ... release anything this task holds ...
        return Done();
    }
    // ... normal work ...
    return Stay();
}
```

체크가 step이 플래그를 읽는 것이므로, 어디서 멈춰도 안전한지를 개발자가 정한다 - 모션 도중이 아니라 모션 사이에서. 모듈 여러 개를 질서있게 내리려면, 플래그를 세팅하고 펌프를 깨운 뒤 각각 `WaitUntilIdle()` 을 부른다:

```cpp
GlobalEnv::RequestStop();
rt.Wake();
for (auto* m : all_modules)
{
    m->WaitUntilIdle();
}
```

---

## 챕터 13. 모듈 하나에 태스크 여럿 - flow를 단위로 묶기

1-12장은 매번 태스크 한 개짜리 모듈을 썼다. 짧은 flow에는 그것이 맞다. 하지만 실제 장비 시퀀스는 15, 20, 30개 step이고, 30개 step의 평평한 체인은 아무리 이름이 좋아도 유지보수에서 중요한 질문에 답하지 못한다: **이 step은 어느 동작에 속하고, 그 동작은 어디서 시작해 어디서 끝나는가?**

이 메커니즘은 이미 갖춰져 있다: 모듈은 여러 태스크를 가질 수 있고, 각각이 `struct Task_X : uniflow::Task<Flow>` 이다. **태스크**는 이름 있는 단위 동작 - 함께 하나의 의미 있는 일(하나의 *Pick*, 하나의 *Place*, 하나의 *Prepare*)을 하는 step들의 묶음 - 이며, 그 묶음은 네이밍 관례가 아니다. 컴파일러가 강제한다. step이 자기 태스크 타입의 멤버이기 때문이다.

### 여러 단위 선언

각 태스크는 public 멤버 struct이다. 모듈 생성자가 태스크마다 `AddTask` 를 한 번씩 부른다. 영속 하드웨어/장비 상태는 모듈에 있고(`flow()` 로 접근), 일시적인 실행별 상태(세틀 타이머, 재시도 카운터)는 태스크에 있다.

```cpp
class Flow_Loader : public uniflow::Uniflow<Flow_Loader>
{
public:
    explicit Flow_Loader(uniflow::Runtime& rt)
        : uniflow::Uniflow<Flow_Loader>(rt, "Flow_Loader")
    {
        AddTask(ctx_pick_);
        AddTask(ctx_place_);
    }

    // Unit: Pick - approach the source, grip, lift, then hand off to Place.
    struct Task_Pick : uniflow::Task<Flow_Loader>
    {
        StepResult Entry() override { return Step1_MoveToSource(); }

    private:
        StepResult Step1_MoveToSource();
        StepResult Step2_WaitAtSource();
        StepResult Step3_Grip();
        StepResult Step4_Lift();
    } ctx_pick_;

    // Unit: Place - carry to the destination and release.
    struct Task_Place : uniflow::Task<Flow_Loader>
    {
        StepResult Entry() override { return Step1_MoveToDest(); }

    private:
        StepResult Step1_MoveToDest();
        StepResult Step2_WaitAtDest();
        StepResult Step3_Release();
    } ctx_place_;

private:
    Motion* axis_;   // durable hardware, reached as flow().axis_
};
```

step 본문은 태스크로 한정해 out-of-line으로 정의하고, `uniflow::StepResult` 를 반환한다(.cpp에 `using namespace uniflow;` 를 넣으면 접두사를 뗄 수 있다):

```cpp
using namespace uniflow;

// Advance WITHIN the unit with Next.
StepResult Flow_Loader::Task_Pick::Step1_MoveToSource()
{
    Describe("moving to source");
    flow().axis_->Move(kSourceX);
    return Next(UF_FN(Step2_WaitAtSource));
}

// Cross a unit boundary ONLY with StartTask.
StepResult Flow_Loader::Task_Pick::Step4_Lift()
{
    if (flow().axis_->AtPickHeight())
    {
        return StartTask(flow().ctx_place_);   // Pick done -> enter Place
    }
    return Stay();
}
```

### 이제 컴파일러가 강제하는 것

이것이 이 구성의 핵심이다 - 더 이상 규율로 지킬 필요가 없는 불변식들:

1. **step의 소속이 타입에 고정된다.** `Task_Pick::Step1_MoveToSource` 는 `Task_Pick` 의 멤버이다. 어느 step 하나만 읽어도 그 단위를 알 수 있으며, 틀린 그룹으로 이름 붙일 수 없다.
2. **단위 경계가 명시적이다.** `Task_Pick` 안에서 `Next(UF_FN(...))` 는 다른 `Task_Pick` step만 가리킬 수 있다. `UF_FN` 이 이름을 현재 태스크 타입에 대해 해석하기 때문이다. `Task_Place` step을 가리키면 타입 불일치이며 컴파일되지 않는다. 단위를 나가는 유일한 길은 `StartTask` 이며, 그래서 flow가 한 동작에서 다음으로 소리 없이 흘러내릴 수 없다.
3. **진입은 계약이다.** `StartTask(flow().ctx_place_)` 는 `Task_Place::Entry()` 에만 안착한다. 단위의 내부 step은 private이라 밖에서 진입할 수 없으므로, 호출부를 하나도 건드리지 않고 단위 내부를 재배치할 수 있다.

### 단위를 순서대로 잇는 두 방법

step 안의 `StartTask` 는 in-flow 전환이다 - 도는 태스크가 끝나고 다음 태스크가 같은 모듈에서 다음 펌프 라운드에 시작된다. 한 단위가 항상 다음으로 흐를 때 맞는 도구이다.

그러나 더 흔하게는 각 태스크가 `Done()` 으로 끝나고, **오케스트레이터** 가 장비 상태를 보고 다음에 무엇을 돌릴지 결정한다. 그러면 단위는 자기 뒤에 무엇이 오는지 몰라도 된다. 이것이 레퍼런스 예제의 지배적 패턴이며(아래 오케스트레이션 절 참고), `StartTask` 가 상대적으로 드문 이유이다.

### 단위별 transient 상태

한 번의 단위 실행에 속한 상태 - 세틀 타이머, 재시도 카운터, 측정값 - 는 태스크의 멤버가 된다. `OnEnter()` 를 override 해 단위 진입 시 리셋한다; 그러면 단위 안의 `Stay()` 재진입을 견디고 다음 진입에서 리셋된다:

```cpp
struct Task_Prepare : uniflow::Task<Flow_Stage>
{
    void OnEnter() override { settle_.Restart(); }   // reset per-run timer on entry
    StepResult Entry() override { return Step1_SendStart(); }

private:
    uniflow::UFTimer settle_;       // per-run settle timer

    StepResult Step1_SendStart();
    StepResult Step2_WaitReady();
    StepResult Step_Timeout();
} ctx_prepare_;
```

```cpp
StepResult Flow_Stage::Task_Prepare::Step2_WaitReady()
{
    using namespace std::chrono_literals;
    if (settle_.HeldFor(flow().hw_ready_->IsReady(), 50ms))   // settled
    {
        flow().state_ = StageState::Prepared;
        return Done();                                        // orchestrator runs Process next
    }
    return StayTimeout(3000ms, UF_FN(Step_Timeout));            // never settled -> recover
}
```

struct를 모든 step에 손으로 꿰는 것과 비교해 보라. 단위가 자기 작업 상태를 소유하고, 프레임워크가 경계에서 리셋한다.

### 작은 flow는 이 비용을 치르지 않는다

3-step 폴러에 단위 셋은 필요 없다. `Entry()` 하나짜리 단일 태스크 - 1-12장이 쓴 그것 - 이다. 모델이 균일하다: step 한 개짜리 flow와 30-step 3-태스크 flow를 똑같이 쓴다. 그래서 flow가 자랄 때 구조가 거기 있고, 자라지 않을 때 추가 비용이 적다.

> 이 장의 모든 것을 실제로 돌리는 완성본은 [pick_and_place](examples/pick_and_place/)이다: 두 피커가 `Pick -> Place` 태스크 쌍, Stage가 `Prepare -> Process -> Cleanup`, 단위별 타이머·비동기 명령·`StayTimeout` 하드웨어 타임아웃까지. 멀티 태스크 flow의 레퍼런스 읽을거리이다.

---

## 마지막: 다 합쳐서 - 오케스트레이션

여러 모듈이 어느 순서로 무엇을 할지를 결정하는 모듈을 "오케스트레이터" 라고 부른다. 보통 영원히 도는 단일 태스크 하나의 step이 매 라운드 `peer.IsIdle()` 과 장비 상태를 보고 피어에 태스크를 launch 할지 정한다.

```cpp
class Flow_Orchestrator : public uniflow::Uniflow<Flow_Orchestrator>
{
public:
    explicit Flow_Orchestrator(uniflow::Runtime& rt)
        : uniflow::Uniflow<Flow_Orchestrator>(rt, "Flow_Orchestrator")
    {
        AddTask(ctx_schedule_);
    }

    struct Task_Schedule : uniflow::Task<Flow_Orchestrator>
    {
        StepResult Entry() override { return Step1_Tick(); }

    private:
        StepResult Step1_Tick();
    } ctx_schedule_;
};
```

```cpp
using namespace uniflow;

StepResult Flow_Orchestrator::Task_Schedule::Step1_Tick()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }

    // Drive the stage: one task per machining phase, launched as the previous
    // phase completes. The stage never sequences itself.
    Flow_Stage& stage = App::inst().stage;
    if (stage.IsIdle())
    {
        switch (stage.state())
        {
        case StageState::RawPartLoaded:
            stage.ctx_prepare_.StartFlow();
            break;
        case StageState::Prepared:
            stage.ctx_process_.StartFlow();
            break;
        case StageState::Machined:
            stage.ctx_cleanup_.StartFlow();
            break;
        default:
            break;
        }
    }

    return Stay();   // poll the line forever
}
```

각 태스크는 `Done()` 으로 끝나며 모듈 상태를 전진시키고, 오케스트레이터의 단일 루프 step이 그 상태를 읽어 모듈이 idle일 때 다음 태스크를 launch 한다. 피커와 스테이지는 자기 순서를 정하지 않으며, 오케스트레이터가 소유한다.

이것이 거의 모든 실제 uniflow 프로그램의 구조이다: [pick_and_place의 Flow_Orchestrator](examples/pick_and_place/uf_orchestrator.cpp)(두 피커와 스테이지를 구동하는 단일 `Schedule` 태스크), 그리고 [message_dispatch](examples/message_dispatch/)의 스포너 태스크들(메일박스가 비지 않으면 소비자를 launch).

---

## 그 다음

- [EXAMPLES.kr.md](EXAMPLES.kr.md) - 동작하는 예제들 둘러보기.
- [uniflow.hpp](uniflow.hpp) - 헤더 자체. 핵심 클래스마다 주석이 상세하다. `uniflow::kVersion` 은 `"1.0.0"`.
- 같은 예제가 [Python](../python/examples) 과 [C#](../cs/examples) 에도 있다.

질문 / 버그 / 패치는 issue 또는 PR로 받는다.
