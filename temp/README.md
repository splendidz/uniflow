# uniflow-cpp

**A single-threaded, run-to-completion cooperative async runtime for C++17.**
*C++17 단일 스레드 · run-to-completion 협동형 비동기 런타임.*

---

## English

### What it is

`uniflow` lets one thread drive many independent flows safely. Each module
expresses its logic as a *chain of step functions*; the runtime pumps every
module round-robin, and blocking work is pushed to a thread pool with the
result delivered to the next step. The main thread is never blocked and never
shares mutable state across threads — data races are designed out, not
guarded against.

It fits any logic that is naturally a sequence of ordered steps with blocking
calls in between — request handlers, job pipelines, connection lifecycles,
workflow engines — and especially anywhere step order matters and "how far
did the flow get before it failed" must be answerable. See [DESIGN.md](DESIGN.md)
for the full rationale and the alternatives that were rejected.

### How execution works

A module's logic is a **flow** — a chain of step functions. The runtime does
not call those steps in a loop directly; instead three layers call inward:

```
   UniflowRuntime::Run()       the pump loop — one thread, the only thread
        │   each round, for every module with a flow still running:
        ▼
   module.ExecuteOnce()        advances that module by exactly one step
        │   calls the module's current step function
        ▼
   StepResult OnSomeStep()     your code — it runs, then returns an intent:

        ├─ UF_NEXT(OnNextStep)   advance — runtime moves the cursor;
        │                                 the next step runs next round
        ├─ Stay()                hold    — cursor unchanged; this same
        │                                 step runs again next round
        └─ Done() / Fail()       end     — the flow finishes; the module
                                           goes idle and is skipped
```

So the runtime runs **one step per module per round**, then moves on. A step
never blocks: it runs, returns an intent, and control is back at the pump.

**Two flows share the one thread.** Say module A has a 3-step flow and module
B has a 2-step flow. The pump visits them turn by turn, round after round:

```
   round 1    A: run A-step-1  →  advance
              B: run B-step-1  →  stay      (not ready yet)

   round 2    A: run A-step-2  →  advance
              B: run B-step-1  →  advance   (re-run — now ready)

   round 3    A: run A-step-3  →  done      (module A → idle)
              B: run B-step-2  →  done      (module B → idle)

   round 4    both modules idle — the pump sleeps until new work arrives
```

Read in call order, the two flows interleave on the single thread:

```
   A-step-1 → B-step-1 → A-step-2 → B-step-1 → A-step-3 → B-step-2
                         └ B-step-1 ran twice: in round 1 it returned
                           `stay`, so round 2 simply ran it again
```

No flow can stall another — each step hands control straight back — and
genuinely blocking work is offloaded to a thread pool rather than run inside
a step (see the `02_async` tutorial).

### Project layout

```
uniflow-cpp/
├── include/
│   ├── uniflow.hpp          the framework — header-only, C++17
│   └── BS_thread_pool.hpp   vendored third-party pool (optional, see below)
├── examples/
│   ├── demo/                realistic multi-module demo (6 scenarios)
│   └── tutorials/           bite-size, single-file lessons
├── vs2022/                  Visual Studio 2022 solution + projects
├── notes/                   historical design notes
├── build/                   build output (git-ignored)
├── DESIGN.md                design rationale and decision history
└── README.md
```

### Quick start

All commands run from the repository root. Build output goes to `build/`.

**Visual Studio 2022** — open `vs2022/uniflow-cpp.sln`, pick a project
(`demo`, `01_minimal`, `02_async`, `03_polling`), and build (x64).

**MSVC command line:**

```cmd
cl /std:c++17 /EHsc /O2 /I include ^
   examples\tutorials\01_minimal.cpp /Fe:build\01_minimal.exe

cl /std:c++17 /EHsc /O2 /I include /DUNIFLOW_USE_BS_THREAD_POOL ^
   examples\demo\main.cpp examples\demo\order_module.cpp examples\demo\session_module.cpp ^
   /Fe:build\demo.exe
```

**g++ / clang++:**

```bash
g++ -std=c++17 -O2 -pthread -I include \
    examples/demo/main.cpp examples/demo/order_module.cpp examples/demo/session_module.cpp \
    -o build/demo
```

### Examples

- **`examples/demo/`** — an order-checkout module and a connection-session
  module driven through six scenarios (success, early-exit jump, validation
  failure, abort, and two modules running concurrently on one thread).

Framework basics — one file each:
- **`01_minimal.cpp`** — the smallest module: a flow, a step chain,
  `Done()` / `Fail()`. No async.
- **`02_async.cpp`** — offloading a blocking call with `UF_ASYNC` and
  receiving the result with `AsyncResult<T>()`.
- **`03_polling.cpp`** — `Stay()`: re-running a step until a condition holds.

Classic concurrency problems — **lock-free here**, because one cooperative
thread makes every step an implicit critical section:
- **`04_producer_consumer.cpp`** — a shared bounded buffer with no mutex and
  no condition variables.
- **`05_dining_philosophers.cpp`** — the textbook deadlock problem, made
  structurally deadlock-proof (both forks taken atomically in one step).
- **`06_bank_transfer.cpp`** — transfers across shared accounts with no
  per-account locking, no lock ordering, and no lost updates.

### Requirements

A C++17 compiler. Verified with MSVC (Visual Studio 2022) and g++ 9+.
The framework is header-only — just put `include/` on the include path.

### Third-party

`include/BS_thread_pool.hpp` is **BS::thread_pool v4.1.0** by Barak Shoshany,
vendored from <https://github.com/bshoshany/thread-pool> (MIT License). It is
**optional**: it is compiled in only when `UNIFLOW_USE_BS_THREAD_POOL` is
defined. Without that define, `uniflow` uses its own bundled `StdThreadPool`
and the file is not needed. The header carries its own license text.

---

## 한국어

### 무엇인가

`uniflow`는 스레드 하나로 여러 독립 흐름을 안전하게 굴리는 프레임워크입니다.
각 모듈은 자기 로직을 *step 함수들의 체인*으로 표현하고, 런타임은 모든 모듈을
round-robin으로 pump합니다. 블로킹 작업은 스레드 풀로 넘기고 결과는 다음
step에서 받습니다. 메인 스레드는 절대 블로킹되지 않고, 스레드 간에 가변 상태를
공유하지 않습니다 — 데이터 레이스를 *막는* 게 아니라 애초에 *불가능하게*
설계했습니다.

순서가 정해진 step들 사이사이에 블로킹 호출이 끼어드는 로직이라면 어디에나
맞습니다 — 요청 핸들러, 작업 파이프라인, 연결 수명주기, 워크플로 엔진 등.
특히 step 순서가 중요하고 "흐름이 어디까지 갔다가 실패했는지"를 답할 수 있어야
하는 경우에 좋습니다. 설계 근거와 검토 후 기각한 대안들은 [DESIGN.md](DESIGN.md)에
정리돼 있습니다.

### 실행 방식

모듈의 로직은 **flow** — step 함수들의 체인 — 하나입니다. 런타임이 그 step들을
직접 루프 돌며 부르지 않습니다. 대신 세 계층이 안쪽으로 호출해 들어갑니다:

```
   UniflowRuntime::Run()       펌프 루프 — 스레드 하나, 유일한 그 스레드
        │   매 라운드, flow가 아직 도는 모든 모듈에 대해:
        ▼
   module.ExecuteOnce()        그 모듈을 정확히 step 하나만큼 전진
        │   모듈의 현재 step 함수를 호출
        ▼
   StepResult OnSomeStep()     당신 코드 — 실행된 뒤 의도(intent)를 반환:

        ├─ UF_NEXT(OnNextStep)   advance — 런타임이 커서를 옮김;
        │                                 다음 라운드에 다음 step 실행
        ├─ Stay()                hold    — 커서 그대로; 이 step을
        │                                 다음 라운드에 다시 실행
        └─ Done() / Fail()       end     — flow 종료; 모듈은 idle이
                                           되고 이후 라운드에선 건너뜀
```

즉 런타임은 **라운드마다 모듈당 step 하나**만 실행하고 넘어갑니다. step은
블로킹하지 않습니다 — 실행 후 의도를 반환하면 제어권이 곧장 펌프로 돌아옵니다.

**두 flow가 한 스레드를 나눠 씁니다.** 모듈 A의 flow는 3-step, 모듈 B의 flow는
2-step이라고 합시다. 펌프는 라운드마다 둘을 번갈아 방문합니다:

```
   round 1    A: run A-step-1  →  advance
              B: run B-step-1  →  stay      (아직 준비 안 됨)

   round 2    A: run A-step-2  →  advance
              B: run B-step-1  →  advance   (재실행 — 이제 준비됨)

   round 3    A: run A-step-3  →  done      (모듈 A → idle)
              B: run B-step-2  →  done      (모듈 B → idle)

   round 4    두 모듈 다 idle — 펌프는 새 작업이 올 때까지 잠듦
```

호출 순서대로 읽으면, 두 flow는 한 스레드 위에서 이렇게 교차됩니다:

```
   A-step-1 → B-step-1 → A-step-2 → B-step-1 → A-step-3 → B-step-2
                         └ B-step-1 이 두 번 실행됨: round 1에서 stay를
                           반환했기에 round 2가 다시 실행
```

어느 flow도 다른 flow를 막지 못합니다 — 각 step이 제어권을 즉시 돌려주니까요.
그리고 진짜 블로킹 작업은 step 안에서 돌리지 않고 스레드 풀로 넘깁니다
(`02_async` 튜토리얼 참고).

### 폴더 구조

```
uniflow-cpp/
├── include/
│   ├── uniflow.hpp          프레임워크 — 헤더 온리, C++17
│   └── BS_thread_pool.hpp   벤더링한 외부 풀 (선택 사항, 아래 참고)
├── examples/
│   ├── demo/                실전형 멀티 모듈 데모 (6개 시나리오)
│   └── tutorials/           한 파일짜리 입문 예제
├── vs2022/                  Visual Studio 2022 솔루션 + 프로젝트
├── notes/                   초기 설계 메모 (역사적 기록)
├── build/                   빌드 산출물 (git 무시)
├── DESIGN.md                설계 근거와 결정 이력
└── README.md
```

### 빠른 시작

모든 명령은 저장소 루트에서 실행합니다. 빌드 산출물은 `build/`로 들어갑니다.

**Visual Studio 2022** — `vs2022/uniflow-cpp.sln`을 열고 프로젝트
(`demo`, `01_minimal`, `02_async`, `03_polling`)를 골라 x64로 빌드합니다.

**MSVC 커맨드라인:**

```cmd
cl /std:c++17 /EHsc /O2 /I include ^
   examples\tutorials\01_minimal.cpp /Fe:build\01_minimal.exe

cl /std:c++17 /EHsc /O2 /I include /DUNIFLOW_USE_BS_THREAD_POOL ^
   examples\demo\main.cpp examples\demo\order_module.cpp examples\demo\session_module.cpp ^
   /Fe:build\demo.exe
```

**g++ / clang++:**

```bash
g++ -std=c++17 -O2 -pthread -I include \
    examples/demo/main.cpp examples/demo/order_module.cpp examples/demo/session_module.cpp \
    -o build/demo
```

### 예제

- **`examples/demo/`** — 주문 체크아웃 모듈과 연결 세션 모듈을 6개 시나리오로
  구동 (성공, 조기 종료 점프, 검증 실패, abort, 두 모듈을 한 스레드에서 동시 실행).

프레임워크 기본 — 한 파일짜리:
- **`01_minimal.cpp`** — 가장 작은 모듈: flow 하나, step 체인,
  `Done()` / `Fail()`. async 없음.
- **`02_async.cpp`** — `UF_ASYNC`로 블로킹 작업을 풀에 넘기고
  `AsyncResult<T>()`로 결과 받기.
- **`03_polling.cpp`** — `Stay()`: 조건이 충족될 때까지 같은 step을 다시 실행.

고전 동시성 문제 — **여기선 락이 없습니다**. 협동형 단일 스레드라 모든 step이
암묵적 임계 구역이 되기 때문입니다:
- **`04_producer_consumer.cpp`** — 공유 bounded buffer를 mutex·condition
  variable 없이.
- **`05_dining_philosophers.cpp`** — 교과서적 데드락 문제를, 구조적으로
  데드락 불가능하게 (양쪽 포크를 한 step에서 원자적으로 집음).
- **`06_bank_transfer.cpp`** — 공유 계좌 간 이체를 계좌별 락도, 락 순서도,
  잃어버린 갱신(lost update)도 없이.

### 요구 사항

C++17 컴파일러. MSVC(Visual Studio 2022)와 g++ 9+에서 검증했습니다.
프레임워크는 헤더 온리라 `include/`를 include 경로에 넣기만 하면 됩니다.

### 외부 라이브러리

`include/BS_thread_pool.hpp`는 Barak Shoshany의 **BS::thread_pool v4.1.0**으로,
<https://github.com/bshoshany/thread-pool>에서 가져왔습니다(MIT License).
**선택 사항**입니다 — `UNIFLOW_USE_BS_THREAD_POOL`를 정의했을 때만 컴파일에
포함됩니다. 정의하지 않으면 `uniflow`는 자체 번들 `StdThreadPool`을 쓰며 이
파일은 필요 없습니다. 라이선스 전문은 헤더 파일 안에 포함돼 있습니다.
