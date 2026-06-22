# city_traffic

> Language: [한국어](README.kr.md) | **English** | [<- Example gallery](../../EXAMPLES.md)

<p align="center">
  <img src="../../../.res/city_traffic.gif" alt="city_traffic demo" width="720"/>
</p>

A comprehensive uniflow example. Fifteen cars, each an independent uniflow
module, accelerate/stop/turn around a city indefinitely while watching shared
traffic lights and the car ahead.

The defining property: there are **zero application-level threads**. Every car,
every traffic light, and the render snapshot update all run cooperatively on a
single pump thread. The shared `World` holding vehicle positions and signal
state has no locks at all - only one thread ever touches it.

---

## Feature focus

- **Dozens of modules cooperating on one thread.** 15 cars + intersection lights
  + the visualization module all run round-robin on one pump, with no
  multithreading.
- **Lock-free shared state.** An immutable `Map` (road network) and a mutable
  `World` (vehicle registry + signal table) are shared by every module, yet the
  only mutex is the pump->render snapshot hand-off ([snapshot.h](snapshot.h)).
  Modules read each other with plain member access.
- **Pure cooperative scheduling, no async.** This example has no blocking work.
  All progress is a `Stay()` tick loop that integrates physics each round using
  real elapsed time (dt). Dozens of flows advance without async.
- **State machine as a module.** A car's driving stages are a step chain:
  `Step_Cruise` (accelerate / brake at the stop line) -> `Step_Wait` (hold on
  red) -> `Step_Cross` (cross the junction) -> `Step_Turn` (turn arc) -> loop.
  Signal phases are step transitions too, so they show up in the trace.
- **Dual renderer (one data, two back-ends).** The pump copies the `World` into a
  `Snapshot`; the render side draws that same snapshot one of two ways - a Win32
  GDI window on Windows, an ANSI console elsewhere (Linux/macOS). The pump logic
  and the render back-end are separated.

---

## The model

| Element | Role |
|---|---|
| `Flow_Vehicle` | One car = one module. Position is a directed edge (from -> to) plus a fraction dist in [0,1]. Acceleration, stop-line braking, car-ahead following (`GapAhead`), and probabilistic turns are expressed as steps |
| `Flow_TrafficLight` | One per intersection (non-corner node). A 2-phase fixed cycle (NS green -> NS yellow -> EW green -> EW yellow). Straight and right turns go together on the green. The starting phase is derived from the node id to desync |
| `Flow_Visualization` | Copies the `World` into a `Snapshot` every tick (pump side); rendering happens on the main thread |
| `World` (shared) | Vehicle position registry + signal table. Lock-free because of the single pump |
| `Map` (immutable) | A fixed, closed road network of nodes/edges. No dead ends -> endless circulation |

---

## Files worth reading

- [uf_vehicle.cpp](uf_vehicle.cpp) - the driving step chain: accel/brake, unified stop-line + car-ahead braking, turn decisions, turn arcs
- [uf_traffic_light.cpp](uf_traffic_light.cpp) - the 2-phase signal cycle expressed as steps
- [world.cpp](world.cpp) - shared signal/vehicle registry and entry-decision helpers
- [app.h](app.h) - attaches lights/fleet/visualization to one Runtime; generates car colors via golden-angle HSV
- [uf_visualization.cpp](uf_visualization.cpp) - pump-side snapshot copy + render dispatcher
- [uf_visualization_win32.cpp](uf_visualization_win32.cpp) - Win32 GDI back-end (Windows only)
- [uf_visualization_console.cpp](uf_visualization_console.cpp) - ANSI console back-end (any terminal)

---

## Build / run

One source set compiles both renderers. [RunVisualisation()](uf_visualization.cpp)
picks the back-end for the platform - always console off Windows; on Windows the
Win32 window, unless the `UF_RENDER=console` environment variable is set.

**Linux / macOS (console, nothing to install):**

```sh
cd cpp/examples/city_traffic
g++ -std=c++17 -O2 -I../.. *.cpp -o city_traffic -pthread
./city_traffic            # ANSI map; press Enter to quit
```

**Windows (MSVC, x64 Native Tools prompt):**

```bat
cd cpp\examples\city_traffic
cl /std:c++17 /EHsc /O2 /I..\.. *.cpp /Fe:city_traffic.exe user32.lib gdi32.lib
city_traffic.exe          :: Win32 window
set UF_RENDER=console & city_traffic.exe   :: force console (preview)
```

> In Win32 mode the default `ConsoleObserver` streams signal phase changes and
> each car's Cruise/Wait/Cross/Turn transitions to the console so the progress is
> visible in the console. In console mode the renderer owns the screen, so a
> silent observer is applied automatically.

---

## Read more

- Cooperative scheduling and lock-free sharing basics: [TUTORIAL.md](../../TUTORIAL.md)
- Full example gallery: [EXAMPLES.md](../../EXAMPLES.md)
