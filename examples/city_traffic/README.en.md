# city_traffic

> 🌐 Language: [한국어](README.md) | **English** &nbsp;|&nbsp; [<- Example gallery](../../EXAMPLES.en.md)

<p align="center">
  <img src="../../docs/videos/city_traffic.gif" alt="city_traffic demo" width="720"/>
</p>

uniflow's grand finale demo. Dozens of cars, each an independent uniflow module, accelerate/stop/turn around a city forever while watching shared traffic lights and the car ahead.

There is one point that matters: there are **zero application-level threads**. Every car, every traffic light, and the render snapshot update all run cooperatively on a single pump thread. The shared `World` holding vehicle positions and signal state has no locks at all - only one thread ever touches it.

---

## What it shows

- **Dozens of modules cooperating** - 15 cars + intersection lights + the visualization module all run round-robin on one pump. A "city running at once" with no multithreading.
- **Lock-free shared state** - an immutable `Map` (road network) and a mutable `World` (vehicle registry + signal table) are shared by every module, yet there is not a single mutex.
- **Pure cooperative scheduling, no UF_ASYNC** - this demo has no blocking work. All progress is a `Stay()` tick loop that integrates physics each round using real elapsed time (dt). Dozens of flows advance smoothly without async.
- **State machine as a module** - a car's driving stages are a step chain: `OnSeg_Cruise` (accelerate / brake at the stop line) -> `OnSeg_Wait` (hold on red) -> `OnSeg_Cross` (cross the junction) -> `OnSeg_Turn` (turn arc) -> loop. Signal phases are step transitions too, so they auto-log to the console.
- **Win32 GDI visualization** - the main thread reads a `Snapshot` and renders roads/signals/cars. The pump thread and the render thread are cleanly separated.

---

## The model

| Element | Role |
|---|---|
| `UF_Vehicle` | One car = one module. Position is a directed edge (from -> to) plus a fraction dist in [0,1]. Acceleration, stop-line braking, car-ahead following (`GapAhead`), and probabilistic turns are expressed as steps |
| `UF_TrafficLight` | One per intersection (non-corner node). A 2-phase fixed cycle (NS green -> NS yellow -> EW green -> EW yellow). Straight and right turns go together on the green. The starting phase is derived from the node id to desync |
| `UF_Visualization` | Reads a `Snapshot`, renders with Win32 GDI, runs the main message loop |
| `World` (shared) | Vehicle position registry + signal table. Lock-free because of the single pump |
| `Map` (immutable) | A fixed, closed road network of nodes/edges. No dead ends -> endless circulation |

Each car publishes its center position to the `World` every tick, and car-ahead detection brute-force scans the cars on the same edge (negligible cost at this scale). Junction-entry decisions read the signal table in the `World`.

---

## Files worth reading

- [uf_vehicle.cpp](uf_vehicle.cpp) - the driving step chain: accel/brake, unified stop-line + car-ahead braking, turn decisions, bezier turn arcs
- [uf_traffic_light.cpp](uf_traffic_light.cpp) - the 2-phase signal cycle expressed as steps
- [world.cpp](world.cpp) - shared signal/vehicle registry and entry-decision helpers
- [app.h](app.h) - attaches lights/fleet/visualization to one Runtime; generates car colors via golden-angle HSV
- [map.cpp](map.cpp) - the node/edge road-network table
- [uf_visualization.cpp](uf_visualization.cpp) - Win32 GDI render (roads, signal arrows, body+wheels+blinkers)

---

## Build / run

It uses Win32 GDI, so this is a Windows + MSVC example. `user32`/`gdi32` are linked via `#pragma comment(lib, ...)` in the source.

```powershell
# inside a vcvars64 environment
cl /std:c++17 /EHsc /I . examples\city_traffic\*.cpp /Fe:build\city_traffic.exe /Fo:build\
build\city_traffic.exe
```

A city window opens, and the default `ConsoleObserver` streams signal phase changes and each car's Cruise/Wait/Cross/Turn transitions so you can feel the simulation running:

```text
[UF_TrafficLight] OnLight_NsGo -> OnLight_EwGo   #03 elapsed=4500.12ms  ...
[UF_Vehicle     ] OnSeg_Cruise -> OnSeg_Wait     #11 elapsed=820.40ms   ...
[UF_Vehicle     ] OnSeg_Wait -> OnSeg_Cross      #12 elapsed=1200.05ms  ...
[UF_Vehicle     ] OnSeg_Cross -> OnSeg_Turn      #13 elapsed=210.33ms   ...
```

Close the window and `App::Shutdown()` requests a stop, waits for every module to go idle, then exits.

---

## Read more

- Cooperative scheduling and lock-free sharing basics: [TUTORIAL.en.md chapters 3, 5](../../TUTORIAL.en.md)
- Orchestration / state-polling pattern: [TUTORIAL.en.md final chapter](../../TUTORIAL.en.md)
- Full example gallery: [EXAMPLES.en.md](../../EXAMPLES.en.md)
