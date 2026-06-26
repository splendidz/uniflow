# pick_and_place

> Language: [한국어](README.kr.md) | **English** &nbsp;|&nbsp; [<- Example gallery](../../EXAMPLES.md)

<p align="center">
  <img src="../../../.res/pick_and_place.gif" alt="pick_and_place demo" width="720"/>
</p>

uniflow's reference project, and the canonical demonstration of **Task-Level Syntax**. It runs a virtual pick-and-place line on a single pump thread: a Load picker grabs raw stock from zone A and places it at zone B, a Stage machines it at B, and an Unload picker carries the finished part from B out to zone C.

It is built around a common line-engineering constraint - **two pickers must never occupy zone B at the same time** - solved with an orchestrator and inter-module state polling. Because every module lives on the same pump, the zone-occupancy check is a plain member read, not a lock.

---

## Why this is the reference project

A picker is not a flat list of moves; it is two *meaningful operations* - **Pick** (acquire the part at the source zone) and **Place** (deliver it at the destination) - each made of several motion steps. That is the shape Task-Level Syntax exists to express, so the pickers read the way the machine actually works:

```text
Flow_LoadPicker   :  Pick (zone A)  ->  Place (zone B)
Flow_UnloadPicker :  Pick (zone B)  ->  Place (zone C)
Flow_Stage        :  Prepare  ->  Process  ->  Cleanup
```

Each task is a `struct ... : uniflow::Task<FlowClass>` that holds the state its steps share AND owns the step member functions; the flow holds one instance (`ctx_pick_`, `ctx_place_`), declared *public* so a peer can launch it. A step is a *member of its task*, so it inherently belongs to that task; `Next` only reaches sibling steps of the same task struct, and crossing to another task is an explicit `StartTask`. Each task names its entry step by overriding `StepResult Entry() override { return Step_First(); }`. A module runs **one task at a time**: anyone launches a task with `module.ctx_x_.StartFlow()` (or `module.StartTask(module.ctx_x_)`), which returns `StartResult` - `Ok` or `Busy` (a task is already running). The orchestrator drives the line by waiting until a module is idle and launching its next task (`Pick` then `Place`; `Prepare` then `Process` then `Cleanup`) - so the *sequence* is explicit and lives in one place, and each task is a self-contained operation that ends with `Done()`. `Task<>` also carries common per-task info for free - `Name()`, `Elapsed()` since entry, and the `Trajectory()` of steps visited (each a `{name, ms held, tick count}`) - and an `OnEnter()` hook to reset the task's own members. Read more in the main [README's Task-Level Syntax section](../../../README.md#task-level-syntax).

This example uses the **direct-call API** rather than per-step macros: steps are plain task member functions that call `Next(...)`, `StayTimeout(...)`, `SubmitAsync(...)` directly and reach the parent flow's state through `flow()` (e.g. `flow().x_->Move(...)`). The only macro is one argument helper - `UF_FN(fn)` (inside a step it expands to `&Task::fn, "fn"`, the member-pointer + log-name pair) - so the functions stay visible to the reader and to IntelliSense.

---

## What it shows

- **Task-Level Syntax in practice** - each picker is a `Pick -> Place` task pair; the Stage is `Prepare -> Process -> Cleanup`. A step is a member of its task, so the task it belongs to is inherent, and each task's entry step (the `Entry()` override) is explicit; tasks are launched - one at a time, from anywhere - with `ctx.StartFlow() -> StartResult`.
- **The Cmd -> WaitAt 2-step motion pattern** - every axis/gripper action is a pair: a "command" step that issues `Move(target)` and advances, and a "wait" step that `Stay()`s while polling `InPosition()`. The natural skeleton for equipment motion code in a cooperative world where you never block.
- **Hardware abstracted behind a factory** - the only hardware type is `MotorAxis` (`Move` / `InPosition` / `Position`). A flow owns the few axes it uses as pointers handed out by the `MotorIOFactory` singleton, which integrates every axis on ONE background thread - the application never steps a motor. A `DigitalLatch` IO (the machining-head ready handshake) lives in the same factory.
- **Mutual exclusion without locks** - the Load/Unload pickers check the other's position via `PartnerInZoneB()` before entering zone B. Both run on the same pump thread, so a member read is enough - no mutex, no race.
- **The orchestrator pattern** - `Flow_Orchestrator` (a single perpetual `Schedule` task) coordinates the whole line: raw-part spawning (a fresh part is staged the instant zone A is empty) and launching each module's *next task* when it is idle (`ctx.StartFlow()`, state-driven on `picker.Carrying()` / `stage.state()`). The pickers and the Stage never decide *which task* runs next on their own.
- **Per-task transient state** - the Stage's `Prepare` task holds a hardware-settle timer and `Process` holds a machining-run timer, as members of those task structs. `OnEnter()` re-arms them when the task is entered, so they span the `Stay()` re-entries within the task.
- **Async commands** - the Stage issues its start/cleanup commands with `SubmitAsync(UF_FN(...))` and picks up the acknowledgement in the next step, all from inside a task.
- **Console + file dual logging** - `EnvLogObserver` mirrors the `ConsoleObserver` output to both the console and `pick_and_place.log`. A real-world custom observer.

---

## The model

| Module | Tasks | Role |
|---|---|---|
| `Flow_LoadPicker` | `Pick`, `Place` | Carries raw stock A -> B |
| `Flow_Stage` | `Prepare`, `Process`, `Cleanup` | Machines at zone B; ready for Unload when done |
| `Flow_UnloadPicker` | `Pick`, `Place` | Carries the machined part B -> C |
| `Flow_Orchestrator` | `Schedule` | Line scheduler: raw-part timing + launching each module's next task |
| `Flow_Visualization` | `Snapshot` | Real-time line visualization (Win32) |

All are held as members of `App` with two-phase init: phase 1 constructs every module (ctor bodies do not touch other modules), and phase 2 `Start()` launches the perpetual tasks (`viz.ctx_snapshot_.StartFlow()`, `orch.ctx_schedule_.StartFlow()`). Each flow wires its tasks with `AddTask(ctx_...)` in its constructor (the entry step is named by each task's `Entry()` override); the orchestrator then launches the work tasks via `ctx.StartFlow()` as each module goes idle.

---

## A step, end to end

A task is a struct (deriving from `uniflow::Task<Flow_LoadPicker>`) that owns the state its steps share and the step member functions; the flow holds one public instance. A step is a member of its task, so it inherently belongs there; it reaches the parent flow's state through `flow()`:

```cpp
public:
    // Each task owns its steps; Entry() names the first step.
    struct Task_Pick : uniflow::Task<Flow_LoadPicker>   // public: the orchestrator launches it
    {
        StepResult Entry() override { return Step1_CmdMoveToSource(); }

    private:                                 // steps are private - only Entry/Next reach them
        StepResult Step1_CmdMoveToSource();  // a Pick step (no context parameter)
        StepResult Step2_WaitAtSource();
        // ...
    } ctx_pick_;

    struct Task_Place : uniflow::Task<Flow_LoadPicker>
    {
        StepResult Entry() override { return Step1_CmdMoveToDest(); }

    private:
        StepResult Step1_CmdMoveToDest();    // a Place step
        // ...
    } ctx_place_;
```

```cpp
// Pick task, command step: issue the move, advance within the task.
StepResult Flow_LoadPicker::Task_Pick::Step1_CmdMoveToSource()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    Describe("cmd: move to zone A");
    flow().x_->Move(GlobalGeometry::kZoneA_mm);   // flow state, reached through flow()
    return Next(UF_FN(Step2_WaitAtSource));       // advance within this task
}

// Pick task, last step: part lifted -> the task is done; the flow goes idle.
StepResult Flow_LoadPicker::Task_Pick::Step7_WaitAtPickUp()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    Describe("lifting with part");
    if (flow().z_->InPosition())
    {
        return Done();   // Pick done; orchestrator launches Place next (sees Carrying())
    }
    return Stay();
}
```

The orchestrator sequences the tasks from outside:

```cpp
auto& p = App::inst().load;
if (p.IsIdle())
{
    if (p.Carrying())                   p.ctx_place_.StartFlow();   // deliver
    else if (GlobalEnv::ZoneAHasPart()) p.ctx_pick_.StartFlow();    // fetch
}
```

A task that shares state declares it in the struct and resets it in `OnEnter()`:

```cpp
struct Task_Prepare : uniflow::Task<Flow_Stage> {
    void OnEnter() override { settle.Restart(); }  // re-armed on task entry
    StepResult Entry() override { return Step1_SendStart(); }
private:
    uniflow::UFTimer settle;
    StepResult Step1_SendStart();
    // ...
} ctx_prepare_;
```

A step reaches the parent flow through `flow()`, so it gets at the borrowed axes (`flow().x_`, `flow().z_`, `flow().finger_`) and peers - and because each task is a nested type of its flow, `flow()` can even read the flow's private members. Sibling state shared by a task's steps lives on the task struct and is reached directly. `Next` only reaches sibling steps of the same task; crossing tasks is an explicit `StartTask`, but here a task simply returns `Done()` and the orchestrator launches the next one. The motor integrates itself on the factory thread, so the wait step only polls `InPosition()` - it never drives the axis.

---

## Files worth reading

- [uf_load_picker.h](uf_load_picker.h) / [.cpp](uf_load_picker.cpp) - the `Pick -> Place` unit pair in full; a clear example of Task-Level Syntax
- [uf_stage.cpp](uf_stage.cpp) - `Prepare -> Process -> Cleanup` with per-unit timers, `SubmitAsync`, and a `StayTimeout` hardware-ready timeout
- [uf_orchestrator.cpp](uf_orchestrator.cpp) - everything about scheduling (spawn / start decisions), as a single `Schedule` unit
- [app.h](app.h) - the two-phase init pattern, Runtime Opts (threads / observer / sleep policy)
- [motor_io_factory.h](motor_io_factory.h) - `MotorAxis` / `DigitalLatch` and the single factory thread that integrates them
- [env_log_observer.h](env_log_observer.h) - a custom observer writing to console + file

---

## Build / run

It uses Win32 visualization, so this is a Windows + MSVC example. A `.vcxproj` is included.

```powershell
# Visual Studio
add cpp\examples\pick_and_place\pick_and_place.vcxproj to your solution and hit F5

# CLI (MSVC, vcvars64 environment)
cl /std:c++17 /EHsc /I cpp cpp\examples\pick_and_place\*.cpp /Fe:build\pick_and_place.exe /Fo:build\
```

A line window opens, and step transitions are written to both the console and `pick_and_place.log`. The log makes the unit structure plainly visible - `Prepare` -> `Process` -> `Cleanup`, `Pick` -> `Place` boundaries - as the line runs. On exit, the delivered part count prints:

```text
parts delivered to Unload: 7
```

---

## Read more

- The Cmd -> WaitAt polling pattern and `Stay()`: [TUTORIAL.md](../../TUTORIAL.md)
- Task-Level Syntax in depth: [TUTORIAL.md](../../TUTORIAL.md) and the main [README](../../../README.md#task-level-syntax)
- Writing a custom observer: [TUTORIAL.md](../../TUTORIAL.md)
- Full example gallery: [EXAMPLES.md](../../EXAMPLES.md)
