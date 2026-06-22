# simulator

> Language: [한국어](README.kr.md) | **English**

A console simulator where several flows share one pump thread and one
**logical clock (VirtualClock)**. Five runners lap their tracks while the user
controls the passage of logical time with commands.

```
  uniflow simulator  v1.0.0
  ------------------------------------------------------------
  [RUNNING]   speed x2.00      pause | resume | speed <n> | quit
  ------------------------------------------------------------
  Atlas    lap  3  [##############......]  72%  Step2_Move
  Bolt     lap  5  [####................]  20%  Step1_Gate
  Comet    lap  2  [####################] 100%  Step3_Rest
  Dash     lap  4  [#########...........]  47%  Step2_Move
  Echo     lap  1  [##......]            ...
  ------------------------------------------------------------
  >
```

## Feature focus

This example is about **time control** and **single-thread cooperation**.

- **VirtualClock - scale / freeze.** Runner progress is measured against
  `Runtime::clock()` (the logical clock), not wall time. `pause` calls
  `clock().Freeze()` once to stop every runner at the same instant; `speed <n>`
  calls `clock().SetScale(n)` once to stretch or compress the whole field. There
  is no per-flow plumbing - one clock governs them all.
  (see `VElapsedMs` in [uf_runner.cpp](uf_runner.cpp), command handling in
  [main.cpp](main.cpp))

- **One thread, no locks.** Five runners plus the renderer are six flows on the
  same pump. A runner writes its row and the renderer reads it, but they share
  the thread and never overlap - which is why [snapshot.h](snapshot.h) has **no
  mutex** (see the header note).

- **The renderer is just a flow.** `Flow_View` in [uf_view.cpp](uf_view.cpp) is
  another module on the same pump. Its frame cadence uses a **real-time**
  `UFTimer`: when the logical clock is frozen (`pause`), the runners stop yet the
  dashboard keeps redrawing and shows `[PAUSED]`.

- **Console / ANSI render pattern.** [console.h](console.h) /
  [console.cpp](console.cpp) is a dependency-free ANSI helper (works on Linux
  terminals and Windows Terminal / Windows 10+). The same pattern is reused by
  the Linux console renderers of city_traffic and pick_and_place.

## Code map

| File | Role |
| --- | --- |
| [uf_runner.h](uf_runner.h) / [.cpp](uf_runner.cpp) | One runner. Loops `Step1_Gate -> Step2_Move -> Step3_Rest` forever (one lap per loop). Progress driven by the VirtualClock. |
| [uf_view.h](uf_view.h) / [.cpp](uf_view.cpp) | Dashboard renderer flow. Draws the snapshot at ~30 real fps. |
| [snapshot.h](snapshot.h) / [.cpp](snapshot.cpp) | Runner -> renderer hand-off (lock-free; reason in the header note). |
| [console.h](console.h) / [.cpp](console.cpp) | Reusable ANSI console helper. |
| [app.h](app.h) | Runtime + every module, two-phase init, silent observer. |
| [main.cpp](main.cpp) | stdin command loop (pause/resume/speed/quit). |

## Commands

| Input | Effect |
| --- | --- |
| `pause` | Freeze logical time (runners stop, dashboard stays live) |
| `resume` | Resume logical time |
| `speed <n>` | Scale logical time (`n > 0`; 0.5 = half, 4 = 4x) |
| `quit` | Stop and exit |

## Build / run

Console only, nothing extra to install.

**Linux / macOS (g++ or clang++):**

```sh
cd cpp/examples/simulator
g++ -std=c++17 -O2 -I../.. *.cpp -o simulator -pthread
./simulator
```

**Windows (MSVC, x64 Native Tools prompt):**

```bat
cd cpp\examples\simulator
cl /std:c++17 /EHsc /O2 /I..\.. *.cpp /Fe:simulator.exe
simulator.exe
```

> Note: this demo assumes an ANSI terminal. On Windows, `console::EnableAnsi()`
> turns on the console's VT processing (Windows Terminal recommended).
