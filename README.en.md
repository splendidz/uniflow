# uniflow-cpp

> 🌐 Language: [한국어](README.md) | **English**

[![ci](https://github.com/splendidz/uniflow-cpp/actions/workflows/ci.yml/badge.svg)](https://github.com/splendidz/uniflow-cpp/actions/workflows/ci.yml)
![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)
![header-only](https://img.shields.io/badge/header--only-single%20file-success)
![dependencies](https://img.shields.io/badge/dependencies-none-success)
![platform](https://img.shields.io/badge/platform-Windows%20%7C%20Linux%20%7C%20macOS-lightgrey)
![license](https://img.shields.io/badge/license-MIT-green)

<p align="center">
  <img src="docs/videos/city_traffic.gif" alt="city_traffic - a city-driving sim on a single pump thread" width="49%"/>
  <img src="docs/videos/cnc_picker.gif" alt="cnc_pickers - a virtual CNC machining line" width="49%"/>
</p>

<p align="center">
  <sub>Left: dozens of cars driving by the signals in <a href="examples/city_traffic/">city_traffic</a> &nbsp;|&nbsp; Right: two pickers running a line with no zone collision in <a href="examples/cnc_pickers/">cnc_pickers</a></sub><br>
  <sub>Both demos use <b>zero application-level threads</b>. Every flow runs cooperatively on a single pump thread.</sub>
</p>

---

## Dozens of flows running at once does not mean you need dozens of threads.

Lock contention, critical sections, deadlocks, and a thread (plus its share of bugs) growing per flow - all of that arrives the moment you reach for multithreading. uniflow puts those flows onto a single-threaded cooperative scheduler as chains of step functions. Instead of simulating tricky timing scenarios in your head, you write the step functions *in the order your thinking flows*. When to yield, when to wake, how to serialize - uniflow handles the concurrency plumbing.

`uniflow-cpp` is a single-header framework that *structurally enforces* the proven **reactor pattern** and the **event loop + worker pool model** (the Node.js / libuv lineage) on top of the C++ type system. It is not a new paradigm; it just packages a long-proven concurrency model in the shape of step functions and hands it to you.

```text
1 header  |  0 external deps  |  C++17  |  no build system required
```

- **Single header** - drop `uniflow.hpp` on your include path and you are done. No CMake/vcpkg/Conan required.
- **Zero external deps** - standard library only. The thread pool (`BS::thread_pool`) is inlined into the header.
- **Flow reads like the code structure** - reading the chain `OnRoute_Validate -> OnRoute_Charge -> OnRoute_Confirm` is reading the spec.
- **Observability is nearly free** - since all progress reduces to step calls, transitions / step time / wait time are logged with no extra code.

---

## How it works - the 30-second picture

**(A) Traditional thread-per-flow**

<p align="center">
  <img src="docs/diagrams/threads.svg" alt="Traditional thread-per-flow architecture" width="760"/>
</p>

A thread per flow, a mutex on every shared-resource access. *You* are responsible for concurrency correctness. Each new flow adds locks, critical sections, and lifetimes to manage.

**(B) uniflow**

<p align="center">
  <img src="docs/diagrams/uniflow.svg" alt="uniflow single-pump architecture" width="760"/>
</p>

Every module's steps are serialized and run round-robin on a single pump thread. Grow the flows to N and the pump is still one. Only blocking work is isolated to a worker pool; results return via the next step's `AsyncResult<T>()`.

There are only three building blocks:

| Element | Role |
|---|---|
| `Runtime` | 1 pump thread + 1 executor (thread pool). The unit modules attach to |
| step function | A member returning `StepResult`. One stage of a flow. `UF_NEXT` / `Stay` / `Done` / `Fail` say what comes next |
| `UF_ASYNC` | Offload blocking work to the pool. The result arrives in the next step |

---

## Tutorial 1 - many flows at once (round-robin)

Attach two modules to the same `Runtime` and the pump visits each one once per round. Each module advances one step per round, so the two chains run one step at a time, interleaved - as if two threads were running. But there is only one thread.

```cpp
#include "uniflow.hpp"
#include <iostream>

// A no-op observer to silence framework logs and show only our output.
// (With the default ConsoleObserver, the same interleave also shows up
//  as step-transition logs.)
struct Silent : uniflow::IUniflowObserver {};

// Module A: a 3-step chain.
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

// Module B: another 3-step chain.
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

    Alice a{rt};      // attach both to the same runtime
    Bob   b{rt};      // -> they run cooperatively on the same pump thread

    UF_START_FLOW(a, OnA_Step1);
    UF_START_FLOW(b, OnB_Step1);

    a.WaitUntilIdle();
    b.WaitUntilIdle();
}
```

Output - the two chains alternate exactly one step at a time:

```text
[Alice] step 1
        [Bob] step 1
[Alice] step 2
        [Bob] step 2
[Alice] step 3
        [Bob] step 3
```

**What happened** - `UF_NEXT` schedules the next step to run *on the next round*. The pump visits Alice and Bob once each per round, so round 1 runs both step 1s, round 2 both step 2s, and so on. Grow this to 100 modules and the pump is still one - and there is nothing to lock, because it is all one thread.

> Leave the default observer in and the framework prints the same interleave as `FLOW START` / step-transition logs. That "free observability" is right below in [Tutorial 3](#tutorial-3---observability-is-free-observer).

---

## Tutorial 2 - throw slow work to the pool (async)

Call a 500ms task directly inside a step body and the whole pump stalls for those 500ms. The fix: hand it to the thread pool with `UF_ASYNC` and pick up the result in the *next step*. While the task runs, the pump keeps driving other modules.

```cpp
#include "uniflow.hpp"
#include <chrono>
#include <iostream>
#include <thread>

struct Silent : uniflow::IUniflowObserver {};

// A module that offloads a slow computation, then uses the result.
class Worker : public uniflow::Uniflow<Worker>
{
    UF_UNIFLOW_IMPLEMENT(Worker);
public:
    explicit Worker(uniflow::Runtime& rt) : uniflow::Uniflow<Worker>(rt) {}

    StepResult OnWork_Begin()
    {
        std::cout << "[worker] submitting slow job (pump is NOT blocked)\n";
        UF_ASYNC(SlowSquare, 9);          // runs on a pool thread
        return UF_NEXT(OnWork_Done);      // result arrives in the next step
    }
private:
    StepResult OnWork_Done()
    {
        auto r = AsyncResult<int>();
        if (r.failed()) return Fail();
        std::cout << "[worker] result in: 9 * 9 = " << r.value() << "\n";
        return Done();
    }

    // A UF_ASYNC target must be static - it runs on another thread, so it
    // cannot touch instance members (enforced at compile time). Inputs are
    // copied in as arguments.
    static int SlowSquare(int n)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500)); // pretend slow
        return n * n;
    }
};

// A heartbeat on the same pump - proof the 500ms job never blocked it.
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
            std::cout << "        [heartbeat] still ticking (" << ++beats_ << ")\n";
        }
        return Stay();                    // re-run next round; pump naps in between
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

Output (timing approximate) - the heartbeat never stops during the 500ms the job spends in the pool:

```text
[worker] submitting slow job (pump is NOT blocked)
        [heartbeat] still ticking (1)
        [heartbeat] still ticking (2)
        [heartbeat] still ticking (3)
        [heartbeat] still ticking (4)
[worker] result in: 9 * 9 = 81
        [heartbeat] still ticking (5)
```

**Key idea** - `UF_ASYNC(SlowSquare, 9)` runs `SlowSquare` on a pool thread and returns immediately. `Worker` advances to `OnWork_Done` to await the result but *does not hold the pump*, so `Heartbeat` keeps ticking on the same thread. The result comes back via `AsyncResult<int>()`, where failures and timeouts branch at the same spot.

> More - timeouts (`UF_ASYNC_TIMEOUT`), error handling, two back-to-back asyncs in one flow: [TUTORIAL.en.md chapters 6-7](TUTORIAL.en.md).

---

## Tutorial 3 - observability is free (observer)

Since all progress takes the single form *step call = function entry*, one measurement hook in the pump loop measures every module and every flow. You write zero trace code.

### Just leave the defaults on - everything is logged

Drop the `Silent` observer from the tutorials above and build the `Runtime` with defaults; the built-in `ConsoleObserver` records the whole flow.

```cpp
#include "uniflow.hpp"
#include <chrono>
#include <thread>

// fetch -> parse -> store. Not a single line of measurement code.
class Pipe : public uniflow::Uniflow<Pipe>
{
    UF_UNIFLOW_IMPLEMENT(Pipe);
public:
    explicit Pipe(uniflow::Runtime& rt) : uniflow::Uniflow<Pipe>(rt) {}

    StepResult OnPipe_Fetch()
    {
        Describe("downloading report");   // an optional one-liner for the log
        UF_ASYNC(Download, 0);
        return UF_NEXT(OnPipe_Parse);
    }
private:
    StepResult OnPipe_Parse()
    {
        int rows = AsyncResult<int>().value();
        Describe("parsed ", rows, " rows");
        return UF_NEXT(OnPipe_Store);
    }
    StepResult OnPipe_Store()
    {
        Describe("committed to db");
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
    uniflow::Runtime rt;                  // default observer = ConsoleObserver
    Pipe p{rt};
    UF_START_FLOW(p, OnPipe_Fetch);
    p.WaitUntilIdle();
}
```

Output - step transitions, async start/finish with wait time, per-step run time, and the flow summary, all automatic (values illustrative):

```text
[Pipe] FLOW START  caller=obs.cpp:34 main()
[Pipe] (entry)                       ASYNC SUBMIT  Download
[Pipe] (entry) -> OnPipe_Parse       downloading report   #00 elapsed=1.77ms  tick x1 avg=0.01ms min=0.01ms max=0.01ms
[Pipe]                               ASYNC DONE    Download  wait=40.21ms
[Pipe] OnPipe_Parse -> OnPipe_Store  parsed 1234 rows     #01 elapsed=40.30ms  tick x1 avg=0.02ms min=0.02ms max=0.02ms
[Pipe] OnPipe_Store                  committed to db      #02 elapsed=0.10ms  tick x1 avg=0.01ms min=0.01ms max=0.01ms
[Pipe] FLOW END  DONE  steps=#02  wall=40.43ms  step=0.04ms  async=40.21ms  tick x3 avg=0.01ms min=0.01ms max=0.02ms (OnPipe_Parse)
```

Each line carries *which step moved to which*, *time spent in that step* (`elapsed`), *body run-time stats* (`min`/`max`/`avg`), and the async job's *pool wait time* (`wait`), plus whatever `Describe(...)` left (the entry step shows as `(entry)`). All of it without putting trace code in the step functions.

### Wire it into your monitoring - override only the hooks you want

To feed your own metrics/alerting, inherit `IUniflowObserver`, override *only the callbacks you care about*, and inject it into the `Runtime`.

```cpp
class Metrics : public uniflow::IUniflowObserver
{
public:
    // Every step transition of every module funnels through here.
    void OnStepChanged(std::string_view obj, std::string_view prev_step,
                       std::string_view /*next_step*/, std::string_view /*desc*/,
                       int /*ordinal*/, double elapsed_ms,
                       const uniflow::TickStats& /*ticks*/) override
    {
        // prometheus_step_ms.observe({obj, prev_step}, elapsed_ms);
        ++step_count_;
    }

    // Fires the moment a step exceeds Config::slow_step_threshold_ms (10ms
    // default). Hang your Slack/PagerDuty alert right here.
    void OnSlowCpuStep(std::string_view obj, std::string_view step,
                       double cpu_ms) override
    {
        // alert(obj, step, cpu_ms);   // "this step held the pump too long"
    }

    int step_count_ = 0;
};

int main()
{
    uniflow::Runtime::Opts opts;
    opts.observer = std::make_unique<Metrics>();
    uniflow::Runtime rt{std::move(opts)};
    // ... every module/flow on this runtime is now measured by Metrics
}
```

**Why this is powerful** - in a thread-per-flow model you must plant logging at every branch to know "where is my thread waiting right now". uniflow intercepts step entry/exit in one place, so the measurement point is *not scattered across the code, it is a single spot*. And the state that multithread APM tools usually miss - *"the code is running but no work is progressing"*, i.e. a step sitting unusually long - shows up per step.

> Full hook list (async / slow-async / slow-round / post / link), per-round profiling, and a real console+file dual-output case: [TUTORIAL.en.md chapter 8](TUTORIAL.en.md), [cnc_pickers' EnvLogObserver](examples/cnc_pickers/).

---

## Learn more

| Doc | Contents |
|---|---|
| [TUTORIAL.en.md](TUTORIAL.en.md) | From a 1-step module to multi-runtime orchestration, one concept per chapter (10 chapters) |
| [EXAMPLES.en.md](EXAMPLES.en.md) | Gallery of 6 working examples - links to each example page |
| [DESIGN.md](DESIGN.md) | Rationale for the design decisions, concept evolution, trade-offs |
| [uniflow.hpp](uniflow.hpp) | The header itself (richly commented) |

---

## Examples

All are *finished* projects that run the moment you build them. The two flagships:

### city_traffic - the grand finale

<p align="center">
  <img src="docs/videos/city_traffic.gif" alt="city_traffic demo" width="640"/>
</p>

Dozens of cars, each an independent uniflow module, accelerate/stop/turn around a city while watching shared traffic lights and the car ahead. Each intersection is a module too. **Zero application-level threads** - every car and signal runs on a single pump, and the shared World (vehicle positions + signal table) needs no locks because it is one thread.

[Open page](examples/city_traffic/README.en.md) | [Folder](examples/city_traffic/)

### cnc_pickers - the reference project

<p align="center">
  <img src="docs/videos/cnc_picker.gif" alt="cnc_pickers demo" width="640"/>
</p>

A virtual CNC machining line. The Load picker carries a part A->B, the Stage machines it at B, the Unload picker moves it B->C. An orchestrator keeps the two pickers out of zone B at the same time and runs the line continuously. Shows the *Cmd -> Wait* 2-step motion pattern, modeled virtual-hardware settle time, and console+file dual logging.

[Open page](examples/cnc_pickers/README.en.md) | [Folder](examples/cnc_pickers/)

### The rest

| Example | One line | Difficulty | UI |
|---|---|---|---|
| [shared_ostream](examples/shared_ostream/README.en.md) | Two modules share one ostringstream with no locks (minimal example) | ★☆☆☆☆ | Console |
| [queue_drain](examples/queue_drain/README.en.md) | Producer/consumer queue-drain pattern | ★★☆☆☆ | Win32 |
| [message_dispatch](examples/message_dispatch/README.en.md) | Two sponsors -> one student message dispatch | ★★★☆☆ | Win32 |
| [weather_llm](examples/weather_llm/README.en.md) | KMA XML -> Gemini summary (real async I/O) | ★★★★★ | Console |

The full gallery and a suggested reading order live in [EXAMPLES.en.md](EXAMPLES.en.md).

---

## Fit - when to use it, when not to

**A good fit when**
- Dozens to hundreds of logical flows must progress at once, but each flow *can be cut into short steps* (state machines, equipment sequences, simulations, event handling)
- There is enough shared state between flows that lock management is a burden
- You want to observe/debug flow progress at step granularity
- You cannot touch the build system (one header is all it takes)

**Not a good fit when**
- You need CPU-bound parallelism that *saturates all cores* - a single pump drives one core. Heavy compute can go to the pool via `UF_ASYNC`, but if the essence is large-scale parallel computation, another tool is better.
- A third-party blocking loop that is hard to cut into steps or cannot yield cooperatively (isolate such work with `UF_ASYNC`)
- An ultra-low-latency path where microsecond delays are critical - the cooperative round period becomes the limit

**How it relates to other tools**
- **Boost.Asio / libuv** - the same reactor + worker-pool model. uniflow expresses flows as *step function chains* instead of callback registration, and is a single header with no external deps.
- **State-machine libraries (boost.sml, etc.)** - instead of a transition table, the call order of step functions *is* the state machine. Async orchestration, scheduling, and observability are built into the framework.

---

## Build

Point the include path at this directory and that is all.

**MSVC**
```powershell
cl /std:c++17 /EHsc /I . examples\shared_ostream\*.cpp /Fe:shared_ostream.exe
```

**GCC / Clang**
```bash
g++ -std=c++17 -O2 -pthread -I . examples/shared_ostream/*.cpp -o shared_ostream
```

**Visual Studio**: `examples/*/<name>.vcxproj` works out of the box; `AdditionalIncludeDirectories=..\..\` is all it needs.

### Run it in your browser

To try the quick tutorials with no local setup, open the repo in GitHub Codespaces (badge below) and in the terminal:

[![Open in GitHub Codespaces](https://github.com/codespaces/badge.svg)](https://codespaces.new/splendidz/uniflow-cpp)

```bash
g++ -std=c++17 -O2 -pthread -I . examples/quickstart/tut1_roundrobin.cpp -o tut1 && ./tut1
```

The compilable single-file versions of all three quick tutorials live in [examples/quickstart/](examples/quickstart/), and CI builds/runs them on Windows/Linux/macOS on every commit.

### Portability

- **No build system required** - just an include path. No package manager.
- **C++17 minimum** - verified on MSVC v142+, GCC 9+, Clang 10+.
- **Platform independent** - identical compile on Windows / Linux / macOS. Only the Win32 visualization in some examples is platform-dependent; the framework itself is not.

---

## Other-language ports

- [uniflow.py](uniflow.py) - a Python port. Design notes in [PYTHON_PORT.md](PYTHON_PORT.md).

---

## License

[MIT](LICENSE). The bundled BS::thread_pool is MIT as well.
