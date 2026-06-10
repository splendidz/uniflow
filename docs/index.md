---
title: uniflow
description: Run dozens of concurrent flows on a single thread - a header-only C++17 cooperative-scheduling framework (reactor + worker-pool), zero deps.
---

<p align="center">
  <img src="{{ '/videos/city_traffic.gif' | relative_url }}" alt="city_traffic - a city-driving simulation on a single pump thread" width="49%"/>
  <img src="{{ '/videos/cnc_picker.gif' | relative_url }}" alt="cnc_pickers - a virtual CNC machining line" width="49%"/>
</p>

## Dozens of flows running at once does not mean you need dozens of threads.

Lock contention, critical sections, deadlocks, and a thread (plus its share of bugs) growing per flow - all of that arrives the moment you reach for multithreading. **uniflow** puts those flows onto a single-threaded cooperative scheduler as chains of step functions. You write the step functions in the order your thinking flows; the framework handles yielding, waking, and serialization.

It is the proven **reactor + worker-pool** model (the Node.js / libuv lineage) packaged as step-function chains in a **single C++17 header** with **zero external dependencies**.

```text
1 header  |  0 external deps  |  C++17  |  Windows / Linux / macOS
```

- **Single header** - drop `uniflow.hpp` on your include path. No CMake/vcpkg/Conan.
- **Lock-free by construction** - every step on a Runtime runs on one pump thread, so shared state needs no mutex.
- **Blocking work, isolated** - offload it to a thread pool with `UF_ASYNC`; the result returns in the next step.
- **Observability for free** - step transitions, timings, and waits are logged with no extra code.

## A 10-line taste

```cpp
#include "uniflow.hpp"
#include <iostream>

class Alice : public uniflow::Uniflow<Alice> {
    UF_UNIFLOW_IMPLEMENT(Alice);
public:
    explicit Alice(uniflow::Runtime& rt) : uniflow::Uniflow<Alice>(rt) {}
    StepResult OnA_Step1() { std::cout << "step 1\n"; return UF_NEXT(OnA_Step2); }
private:
    StepResult OnA_Step2() { std::cout << "step 2\n"; return Done(); }
};
```

Two such modules on one Runtime run their step chains interleaved, one step at a time - concurrency without a second thread.

## Live demos

| | |
|---|---|
| **city_traffic** | Dozens of cars, each an independent module, drive a city by shared traffic lights and the car ahead - zero application-level threads. |
| **cnc_pickers** | A virtual CNC line: two pickers run continuously without ever sharing a zone, coordinated by an orchestrator. |

## Get started

- [Read the docs on GitHub](https://github.com/splendidz/uniflow) ([English README](https://github.com/splendidz/uniflow/blob/main/README.md))
- [Tutorials](https://github.com/splendidz/uniflow/blob/main/TUTORIAL.md) - one concept per chapter
- [Examples](https://github.com/splendidz/uniflow/blob/main/EXAMPLES.md) - six working projects
- [Run the quickstart in your browser (Codespaces)](https://codespaces.new/splendidz/uniflow)

Released under the [MIT License](https://github.com/splendidz/uniflow/blob/main/LICENSE).
