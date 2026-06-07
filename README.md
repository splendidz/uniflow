# uniflow-cpp

> 🌐 언어: **한국어** | [English](README.en.md)

[![ci](https://github.com/splendidz/uniflow-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/splendidz/uniflow-cpp/actions/workflows/ci.yml)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![header-only](https://img.shields.io/badge/header--only-single%20file-success)
![dependencies](https://img.shields.io/badge/dependencies-none-success)
![platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey)
![license](https://img.shields.io/badge/license-MIT-green)

<p align="center">
  <img src="docs/videos/city_traffic.gif" alt="city_traffic - 단일 펌프 스레드로 도는 도시 주행 시뮬레이션" width="49%"/>
  <img src="docs/videos/cnc_picker.gif" alt="cnc_pickers - 가상 CNC 가공 라인" width="49%"/>
</p>

<p align="center">
  <sub>왼쪽: 차량 수십 대가 신호를 보며 도는 <a href="examples/city_traffic/">city_traffic</a> &nbsp;|&nbsp; 오른쪽: 픽커 2대가 zone 충돌 없이 라인을 운전하는 <a href="examples/cnc_pickers/">cnc_pickers</a></sub><br>
  <sub>두 데모 모두 <b>애플리케이션 레벨 스레드 0개</b>. 모든 흐름이 단일 펌프 스레드 위에서 협력적으로 돈다.</sub>
</p>

---

## 수십 개의 흐름이 동시에 돌아야 한다고 해서, 수십 개의 스레드가 필요한 것은 아니다.

멀티스레드로 짜는 순간 따라오는 락 경쟁, 임계영역, 데드락, 그리고 흐름마다 불어나는 스레드와 그만큼 불어나는 버그 - uniflow는 이 흐름들을 단일 스레드 협력 스케줄링 위에 step 체인으로 올린다. 당신은 복잡한 타이밍 시나리오를 머릿속에서 시뮬레이션하지 않고, *생각이 흐르는 순서 그대로* step 함수를 적기만 하면 된다. 흐름을 언제 양보하고 언제 깨우고 어떻게 직렬화할지 - 그 동시성의 뒤치다꺼리는 uniflow가 맡는다.

`uniflow-cpp`는 검증된 **reactor 패턴**과 **event loop + worker pool 모델**(Node.js / libuv 계보)을 C++ 타입 시스템 위에 *구조적으로 강제*한 단일 헤더 프레임워크다. 새 패러다임이 아니라, 오래 검증된 동시성 모델을 step 함수라는 형태로 정리해 손에 쥐어 준다.

```text
헤더 1개  |  외부 의존성 0  |  C++17  |  빌드 시스템 비의존
```

- **단일 헤더** - `uniflow.hpp` 하나를 include path에 넣으면 끝. CMake/vcpkg/Conan 무엇도 요구하지 않는다.
- **외부 의존성 0** - 표준 라이브러리만 사용. thread pool(`BS::thread_pool`)도 헤더에 인라인.
- **흐름이 코드 구조 그대로** - `OnRoute_Validate -> OnRoute_Charge -> OnRoute_Confirm` step 체인을 읽는 것이 곧 사양서를 읽는 것.
- **관측성은 거의 공짜** - 모든 진행이 step 호출로 환원되므로 step 전이/수행시간/대기시간이 별도 코드 없이 로깅된다.

---

## 동작 원리 - 30초 그림

**(A) 전통적인 thread-per-flow**

<p align="center">
  <img src="docs/diagrams/threads.svg" alt="Traditional thread-per-flow architecture" width="760"/>
</p>

흐름마다 스레드를 띄우고, 공유 자원 접근마다 mutex를 건다. 동시성의 정확성을 *사용자가 직접* 보장해야 한다. 흐름을 하나 늘릴 때마다 관리할 락/임계영역/수명이 같이 늘어난다.

**(B) uniflow**

<p align="center">
  <img src="docs/diagrams/uniflow.svg" alt="uniflow single-pump architecture" width="760"/>
</p>

모든 모듈의 step이 단일 pump 스레드 위에서 직렬화되어 round-robin으로 실행된다. 흐름을 N개로 늘려도 펌프는 하나다. 블로킹 작업만 worker pool로 격리되고, 결과는 다음 step의 `AsyncResult<T>()`로 회수된다.

핵심 빌딩 블록은 셋뿐이다:

| 요소 | 역할 |
|---|---|
| `Runtime` | pump 스레드 1개 + executor(thread pool) 1개. 모듈이 부착되는 실행 단위 |
| step 함수 | `StepResult`를 돌려주는 멤버 함수. 흐름의 한 단계. `UF_NEXT` / `Stay` / `Done` / `Fail` 로 다음을 지시 |
| `UF_ASYNC` | 블로킹 작업을 pool로 offload. 결과는 다음 step에서 수신 |

---

## 튜토리얼 1 - 흐름 여럿이 동시에 (라운드로빈)

두 모듈을 같은 `Runtime`에 올리면, 펌프가 매 라운드 두 모듈을 한 번씩 방문한다. 각 모듈은 라운드마다 step 하나씩 전진하므로, 두 체인이 한 step씩 번갈아 실행된다 - 마치 두 스레드가 동시에 도는 것처럼. 하지만 스레드는 하나다.

```cpp
#include "uniflow.hpp"
#include <iostream>

// 프레임워크 자체 로그를 끄고 우리 출력만 보기 위한 no-op observer.
// (기본 ConsoleObserver를 쓰면 같은 인터리브가 step 전이 로그로도 찍힌다.)
struct Silent : uniflow::IUniflowObserver {};

// 모듈 A: 3-step 체인.
class Alice : public uniflow::Uniflow<Alice>
{
    UF_UNIFLOW_IMPLEMENT(Alice);
public:
    explicit Alice(uniflow::Runtime& rt) : uniflow::Uniflow<Alice>(rt) {}

    StepResult OnA_Step1() { std::cout << "[Alice] step 1\n"; return UF_NEXT(OnA_Step2); }
private:
    StepResult OnA_Step2() { std::cout << "[Alice] step 2\n"; return UF_NEXT(OnA_Step3); }
    StepResult OnA_Step3() { std::cout << "[Alice] step 3\n"; return Done(); }
};

// 모듈 B: 또 다른 3-step 체인.
class Bob : public uniflow::Uniflow<Bob>
{
    UF_UNIFLOW_IMPLEMENT(Bob);
public:
    explicit Bob(uniflow::Runtime& rt) : uniflow::Uniflow<Bob>(rt) {}

    StepResult OnB_Step1() { std::cout << "        [Bob] step 1\n"; return UF_NEXT(OnB_Step2); }
private:
    StepResult OnB_Step2() { std::cout << "        [Bob] step 2\n"; return UF_NEXT(OnB_Step3); }
    StepResult OnB_Step3() { std::cout << "        [Bob] step 3\n"; return Done(); }
};

int main()
{
    uniflow::Runtime::Opts opts;
    opts.observer = std::make_unique<Silent>();
    uniflow::Runtime rt{std::move(opts)};

    Alice a{rt};      // 두 모듈을 같은 runtime에 부착
    Bob   b{rt};      // -> 같은 펌프 스레드에서 협력 실행

    UF_START_FLOW(a, OnA_Step1);
    UF_START_FLOW(b, OnB_Step1);

    a.WaitUntilIdle();
    b.WaitUntilIdle();
}
```

출력 - 두 체인이 정확히 한 step씩 번갈아 나온다:

```text
[Alice] step 1
        [Bob] step 1
[Alice] step 2
        [Bob] step 2
[Alice] step 3
        [Bob] step 3
```

**무슨 일이 일어났나** - `UF_NEXT`는 *다음 라운드에* 다음 step을 실행하도록 예약한다. 펌프는 한 라운드에 Alice, Bob을 각각 1회 방문하므로, 라운드 1에서 두 모듈의 step 1이, 라운드 2에서 step 2가... 순서대로 인터리브된다. 모듈을 100개로 늘려도 펌프는 그대로 하나고, 공유 자원에 락을 걸 일도 없다 - 전부 같은 스레드 위니까.

> 기본 observer를 그대로 두면 프레임워크가 같은 인터리브를 `FLOW START` / step 전이 로그로도 찍어 준다. 그 "공짜 관측성"은 바로 아래 [튜토리얼 3](#튜토리얼-3---관측은-공짜다-observer)에서.

---

## 튜토리얼 2 - 시간이 걸리는 일은 풀로 던진다 (async)

step 본문에서 500ms짜리 작업을 직접 호출하면 그동안 펌프 전체가 멈춘다. 해법은 `UF_ASYNC`로 thread pool에 던지고 *다음 step*에서 결과를 받는 것. 작업이 도는 동안 펌프는 다른 모듈을 계속 돌린다.

```cpp
#include "uniflow.hpp"
#include <chrono>
#include <iostream>
#include <thread>

struct Silent : uniflow::IUniflowObserver {};

// 느린 계산을 풀로 던지고, 다음 step에서 결과를 쓰는 모듈.
class Worker : public uniflow::Uniflow<Worker>
{
    UF_UNIFLOW_IMPLEMENT(Worker);
public:
    explicit Worker(uniflow::Runtime& rt) : uniflow::Uniflow<Worker>(rt) {}

    StepResult OnWork_Begin()
    {
        std::cout << "[worker] 느린 작업을 풀에 제출 (펌프는 안 막힘)\n";
        UF_ASYNC(SlowSquare, 9);          // pool 스레드에서 실행
        return UF_NEXT(OnWork_Done);      // 결과는 다음 step에서
    }
private:
    StepResult OnWork_Done()
    {
        auto r = AsyncResult<int>();
        if (r.failed()) return Fail();
        std::cout << "[worker] 결과 도착: 9 * 9 = " << r.value() << "\n";
        return Done();
    }

    // UF_ASYNC 타겟은 반드시 static - 다른 스레드에서 도니까 인스턴스
    // 멤버를 만질 수 없다(컴파일 에러로 강제). 입력은 인자로 복사해 넘긴다.
    static int SlowSquare(int n)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500)); // 느린 척
        return n * n;
    }
};

// 같은 펌프 위에서 계속 뛰는 하트비트 - 500ms 작업이 펌프를 막지
// 않았다는 증거.
class Heartbeat : public uniflow::Uniflow<Heartbeat>
{
    UF_UNIFLOW_IMPLEMENT(Heartbeat);
public:
    explicit Heartbeat(uniflow::Runtime& rt) : uniflow::Uniflow<Heartbeat>(rt) {}

    StepResult OnBeat_Tick()
    {
        if (beats_ >= 5) return Done();
        auto now = uniflow::Clock::now();
        if (now - last_ >= std::chrono::milliseconds(100))
        {
            last_ = now;
            std::cout << "        [heartbeat] 아직 돌고 있음 (" << ++beats_ << ")\n";
        }
        return Stay();                    // 다음 라운드에 다시; 펌프는 그동안 쉰다
    }
private:
    int                beats_ = 0;
    uniflow::TimePoint last_  = uniflow::Clock::now();
};

int main()
{
    uniflow::Runtime::Opts opts;
    opts.observer = std::make_unique<Silent>();
    uniflow::Runtime rt{std::move(opts)};

    Worker    w{rt};
    Heartbeat h{rt};

    UF_START_FLOW(w, OnWork_Begin);
    UF_START_FLOW(h, OnBeat_Tick);

    w.WaitUntilIdle();
    h.WaitUntilIdle();
}
```

출력 (타이밍은 근사) - 작업이 풀에서 도는 500ms 동안 하트비트가 멈추지 않는다:

```text
[worker] 느린 작업을 풀에 제출 (펌프는 안 막힘)
        [heartbeat] 아직 돌고 있음 (1)
        [heartbeat] 아직 돌고 있음 (2)
        [heartbeat] 아직 돌고 있음 (3)
        [heartbeat] 아직 돌고 있음 (4)
[worker] 결과 도착: 9 * 9 = 81
        [heartbeat] 아직 돌고 있음 (5)
```

**핵심** - `UF_ASYNC(SlowSquare, 9)`는 `SlowSquare`를 pool 스레드에서 실행하고 즉시 반환한다. `Worker`는 `OnWork_Done`으로 넘어가 결과를 기다리지만 *펌프를 점유하지 않는다*. 그래서 `Heartbeat`가 같은 스레드 위에서 계속 tick할 수 있다. 결과는 `AsyncResult<int>()`로 받고, 실패/타임아웃도 같은 자리에서 분기한다.

> 더 자세히 - 타임아웃(`UF_ASYNC_TIMEOUT`), 에러 처리, 한 flow에서 두 번 연속 async: [TUTORIAL.md 챕터 6-7](TUTORIAL.md).

---

## 튜토리얼 3 - 관측은 공짜다 (observer)

모든 진행이 *step 호출 = 함수 진입*이라는 단일 형태로 통일되어 있으니, 펌프 루프 한 군데에 measurement hook을 거는 것만으로 모든 모듈, 모든 흐름의 측정이 끝난다. 별도 trace 코드를 단 한 줄도 심지 않아도 된다.

### 그냥 기본값으로 두면 - 전부 로깅된다

위 튜토리얼들의 `Silent` observer를 빼고 `Runtime`을 기본값으로 만들면, 내장 `ConsoleObserver`가 흐름 전체를 받아 적는다.

```cpp
#include "uniflow.hpp"
#include <chrono>
#include <thread>

// fetch -> parse -> store. 측정 코드는 한 줄도 없다.
class Pipe : public uniflow::Uniflow<Pipe>
{
    UF_UNIFLOW_IMPLEMENT(Pipe);
public:
    explicit Pipe(uniflow::Runtime& rt) : uniflow::Uniflow<Pipe>(rt) {}

    StepResult OnPipe_Fetch()
    {
        Describe("리포트 다운로드 중");      // 로그/observer에 남길 한 줄 (선택)
        UF_ASYNC(Download, 0);
        return UF_NEXT(OnPipe_Parse);
    }
private:
    StepResult OnPipe_Parse()
    {
        int rows = AsyncResult<int>().value();
        Describe("파싱 완료: ", rows, " rows");
        return UF_NEXT(OnPipe_Store);
    }
    StepResult OnPipe_Store()
    {
        Describe("DB 커밋");
        return Done();
    }
    static int Download(int)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        return 1234;
    }
};

int main()
{
    uniflow::Runtime rt;                  // 기본 observer = ConsoleObserver
    Pipe p{rt};
    UF_START_FLOW(p, OnPipe_Fetch);
    p.WaitUntilIdle();
}
```

출력 - step 전이, async 시작/끝과 대기시간, 각 step 수행시간, flow 결산이 자동으로 (값은 예시):

```text
[Pipe] FLOW START  caller=obs.cpp:34 main()
[Pipe] (entry)                       ASYNC SUBMIT  Download
[Pipe] (entry) -> OnPipe_Parse       리포트 다운로드 중   #00 elapsed=1.77ms  tick x1 avg=0.01ms min=0.01ms max=0.01ms
[Pipe]                               ASYNC DONE    Download  wait=40.21ms
[Pipe] OnPipe_Parse -> OnPipe_Store  파싱 완료: 1234 rows  #01 elapsed=40.30ms  tick x1 avg=0.02ms min=0.02ms max=0.02ms
[Pipe] OnPipe_Store                  DB 커밋              #02 elapsed=0.10ms  tick x1 avg=0.01ms min=0.01ms max=0.01ms
[Pipe] FLOW END  DONE  steps=#02  wall=40.43ms  step=0.04ms  async=40.21ms  tick x3 avg=0.01ms min=0.01ms max=0.02ms (OnPipe_Parse)
```

각 줄에 *어느 step에서 어느 step으로* 옮겨갔는지, *그 step에 머문 시간*(`elapsed`), *본문 실행 시간 통계*(`min`/`max`/`avg`), async 작업의 *pool 대기시간*(`wait`)이 담겨 있다. `Describe(...)`로 남긴 한 줄도 함께 (진입 step은 `(entry)`로 표시된다). 이 모든 게 step 함수에 trace 코드를 넣지 않고 얻어진다.

### 내 모니터링에 연결하기 - 관심 훅만 override

자체 metrics/alert 시스템에 붙이고 싶으면 `IUniflowObserver`를 상속해 *관심 있는 콜백만* 재정의한 뒤 `Runtime`에 주입한다.

```cpp
class Metrics : public uniflow::IUniflowObserver
{
public:
    // 모든 모듈의 모든 step 전이가 여기 한 곳으로 모인다.
    void OnStepChanged(std::string_view obj, std::string_view prev_step,
                       std::string_view /*next_step*/, std::string_view /*desc*/,
                       int /*ordinal*/, double elapsed_ms,
                       const uniflow::TickStats& /*ticks*/) override
    {
        // prometheus_step_ms.observe({obj, prev_step}, elapsed_ms);
        ++step_count_;
    }

    // 임계값(Config::slow_step_threshold_ms, 기본 10ms)을 넘는 순간 자동 fire.
    // Slack/PagerDuty 알람을 여기에 그냥 걸면 된다.
    void OnSlowCpuStep(std::string_view obj, std::string_view step,
                       double cpu_ms) override
    {
        // alert(obj, step, cpu_ms);   // "이 step이 펌프를 너무 오래 잡았다"
    }

    int step_count_ = 0;
};

int main()
{
    uniflow::Runtime::Opts opts;
    opts.observer = std::make_unique<Metrics>();
    uniflow::Runtime rt{std::move(opts)};
    // ... 이 runtime의 모든 모듈/흐름이 Metrics로 측정된다
}
```

**왜 이게 강력한가** - thread-per-flow 모델에서는 "내 스레드가 지금 어디서 무엇을 기다리는가"를 알려고 매 분기마다 직접 로깅을 심어야 한다. uniflow는 펌프 한 곳이 step 진입/이탈을 가로채므로, 측정 지점이 *코드 전체에 흩어지지 않고 단 한 곳*이다. 게다가 보통 멀티스레드에서 APM 도구도 잘 못 잡는 *"코드는 도는데 일은 진행이 안 됨"* 상태 - 특정 step에 비정상적으로 오래 머무는 것 - 까지 step 단위로 드러난다.

> observer 훅 전체 목록(async/slow-async/slow-round/post/link), 라운드 단위 프로파일링, 콘솔+파일 동시 출력 실제 사례: [TUTORIAL.md 챕터 8](TUTORIAL.md), [cnc_pickers의 EnvLogObserver](examples/cnc_pickers/).

---

## 더 배우기

| 문서 | 내용 |
|---|---|
| [TUTORIAL.md](TUTORIAL.md) | 1-step 모듈부터 멀티 runtime 오케스트레이션까지, 한 챕터에 한 개념씩 (10챕터) |
| [EXAMPLES.md](EXAMPLES.md) | 동작하는 예제 6개 갤러리 - 각 예제 페이지로 연결 |
| [DESIGN.md](DESIGN.md) | 설계 결정의 근거, 컨셉 변천, 트레이드오프 |
| [uniflow.hpp](uniflow.hpp) | 헤더 본체 (상세 주석 포함) |

---

## 예제

전부 빌드하면 바로 도는 *완성된* 프로젝트다. 대표작 둘:

### city_traffic - 마지막 피날레

<p align="center">
  <img src="docs/videos/city_traffic.gif" alt="city_traffic demo" width="640"/>
</p>

수십 대의 차량이 각자 독립된 uniflow 모듈로, 공유 신호등과 앞차를 보며 가감속/정지/회전하며 도시를 순환한다. 신호등도 교차로마다 한 모듈씩. **애플리케이션 레벨 스레드는 0개** - 모든 차량과 신호가 단일 펌프 위에서 돌고, 공유 World(차량 위치 + 신호 테이블)는 단일 스레드라 락이 없다.

[페이지 보기](examples/city_traffic/README.md) | [폴더](examples/city_traffic/)

### cnc_pickers - 레퍼런스 프로젝트

<p align="center">
  <img src="docs/videos/cnc_picker.gif" alt="cnc_pickers demo" width="640"/>
</p>

가상 CNC 가공 라인. Load 픽커가 부품을 A->B로, Stage가 B에서 가공, Unload 픽커가 B->C로 옮긴다. 두 픽커가 zone B에 동시에 들어가지 않게 오케스트레이터가 조율하며 라인을 연속 운전한다. 모터/그리퍼 모션의 *Cmd -> Wait* 2-step 패턴, 가상 하드웨어 settle 모델링, 콘솔/파일 동시 로깅까지.

[페이지 보기](examples/cnc_pickers/README.md) | [폴더](examples/cnc_pickers/)

### 나머지 예제

| 예제 | 한 줄 | 난이도 | UI |
|---|---|---|---|
| [shared_ostream](examples/shared_ostream/README.md) | 모듈 둘이 한 ostringstream을 락 없이 공유 (최소 예제) | ★☆☆☆☆ | 콘솔 |
| [queue_drain](examples/queue_drain/README.md) | 송수신 큐 drain 패턴 | ★★☆☆☆ | Win32 |
| [message_dispatch](examples/message_dispatch/README.md) | 두 스폰서 -> 학생 메시지 디스패치 | ★★★☆☆ | Win32 |
| [weather_llm](examples/weather_llm/README.md) | 기상청 XML -> Gemini 요약 (실제 비동기 I/O) | ★★★★★ | 콘솔 |

전체 갤러리와 추천 학습 순서는 [EXAMPLES.md](EXAMPLES.md)에.

---

## 적합성 - 언제 쓰고, 언제 쓰지 말 것

**잘 맞는 경우**
- 수십~수백 개의 논리적 흐름이 동시에 진행되어야 하지만, 각 흐름이 *짧은 step으로 쪼갤 수 있는* 경우 (상태 기계, 장비 시퀀스, 시뮬레이션, 이벤트 처리)
- 흐름 간 공유 상태가 많아 락 관리가 부담인 경우
- 흐름의 진행 상황을 step 단위로 관측/디버깅하고 싶은 경우
- 빌드 시스템을 못 건드리는 환경 (헤더 하나면 끝)

**안 맞는 경우**
- CPU 바운드 병렬 처리로 *코어를 다 써야* 하는 경우 - 단일 pump는 한 코어만 민다. 무거운 계산은 `UF_ASYNC`로 풀에 넘길 수 있지만, 본질이 대규모 병렬 연산이면 다른 도구가 낫다.
- step으로 쪼개기 어렵거나 협력적 yield가 불가능한 서드파티 블로킹 루프 (이런 작업은 `UF_ASYNC`로 격리)
- 마이크로초 단위 지연이 핵심인 초저지연 경로 - 협력 스케줄링의 라운드 주기가 한계가 된다

**다른 도구와의 관계**
- **Boost.Asio / libuv** - 같은 reactor + worker-pool 모델. uniflow는 콜백 등록 대신 *step 함수 체인*으로 흐름을 표현하고, 외부 의존성 없는 단일 헤더라는 점이 다르다.
- **상태 기계 라이브러리(boost.sml 등)** - 전이 테이블 대신 step 함수의 호출 순서가 곧 상태 기계다. 비동기 오케스트레이션과 스케줄링, 관측까지 프레임워크에 내장된 점이 다르다.

---

## 빌드

include path에 본 디렉토리만 지정하면 된다.

**MSVC**
```powershell
cl /std:c++17 /EHsc /I . examples\shared_ostream\*.cpp /Fe:shared_ostream.exe
```

**GCC / Clang**
```bash
g++ -std=c++17 -O2 -pthread -I . examples/shared_ostream/*.cpp -o shared_ostream
```

**Visual Studio**: `examples/*/<name>.vcxproj` 가 즉시 동작. `AdditionalIncludeDirectories=..\..\` 만 설정되어 있으면 충분하다.

### 브라우저에서 바로 실행

설치 없이 퀵 튜토리얼을 돌려보고 싶다면 GitHub Codespaces로 리포를 열고(아래 배지) 터미널에서:

[![Open in GitHub Codespaces](https://github.com/codespaces/badge.svg)](https://codespaces.new/splendidz/uniflow-cpp)

```bash
g++ -std=c++17 -O2 -pthread -I . examples/quickstart/tut1_roundrobin.cpp -o tut1 && ./tut1
```

세 퀵 튜토리얼의 컴파일 가능한 단일 파일은 [examples/quickstart/](examples/quickstart/)에 있고, CI가 매 커밋마다 Windows/Linux/macOS에서 이들을 빌드/실행한다.

### 포터빌리티

- **빌드 시스템 비의존** - include path 설정만으로 충족. package manager 불필요.
- **C++17 최소 사양** - MSVC v142+, GCC 9+, Clang 10+ 에서 검증.
- **플랫폼 독립** - Windows / Linux / macOS 동일 컴파일. 일부 예제의 Win32 시각화만 플랫폼 의존이며, 프레임워크 자체는 무관하다.

---

## 다른 언어 포트

- [uniflow.py](uniflow.py) - Python 포트. 설계 노트는 [PYTHON_PORT.md](PYTHON_PORT.md).

---

## 라이선스

[MIT](LICENSE). 내장된 BS::thread_pool 또한 MIT 라이선스이다.
