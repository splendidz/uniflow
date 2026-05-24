# cnc_pickers refactor - session handoff

Purpose of this doc: a new chat session can read ONLY this file (plus the
files it points to) and pick up the work cleanly. No prior chat context
required.

---

## Status (2026-05-25)

**Split DONE - both header AND source.** Every class now has a
declaration in its `.h` and definitions in a matching `.cpp`. Step
bodies for the uniflow modules live in the `.cpp` (`UF_NEXT`,
`Describe`, etc. resolve fine from out-of-line member definitions
because the macros only depend on `S = Cls` / the CRTP base, both of
which are in scope within a member function).

| Header | Source | Contains |
|---|---|---|
| `globals.h` | `globals.cpp` | GlobalGeometry, GlobalTiming, GlobalEnv, HwSimulator, StageState (+ToString), MotorAxis. constexpr members live in the header; defs go in the .cpp. |
| `snapshot.h` | `snapshot.cpp` | `Snapshot` struct, `extern g_snap`/`g_snap_mu`, `ReadSnapshot()`. |
| `picker_motion.h` | `picker_motion.cpp` | `PickerMotion` non-Uniflow helper base (x_axis_, z_axis_, finger_axis_). |
| `stage.h` | `stage.cpp` | `Stage` (Uniflow singleton). |
| `load_picker.h` | `load_picker.cpp` | `LoadPicker` (Uniflow). |
| `unload_picker.h` | `unload_picker.cpp` | `UnloadPicker` (Uniflow). The cpp also defines `LoadPicker::PartnerInZoneB()` / `UnloadPicker::PartnerInZoneB()` since both classes are visible after including load_picker.h. |
| `orchestrator.h` | `orchestrator.cpp` | `Orchestrator` (Uniflow singleton). |
| `env_log_observer.h` | `env_log_observer.cpp` | `EnvLogObserver` (uniflow::IUniflowObserver subclass). |
| `viz.h` | `viz.cpp` | `Viz` (Uniflow singleton) + Win32/console rendering. `<windows.h>` confined to viz.cpp. |
| (n/a) | `main.cpp` | `InitializeEnv`, `ShutdownEnv`, `main()`. |

`cnc_pickers.vcxproj` + `.filters` list all `.h` and all `.cpp` files.
The console/Win32 cl command lines now pass every `.cpp` (see Build
commands below).

**Orchestrator: hybrid poll + event scheduling.** `OnSchedule_Tick`
still parks on `Wait(kSchedTick)` - 30ms - but that poll is now a
backstop for ONE time-driven decision (creating a raw part when its
randomised gap elapses). The state-driven `Try*` decisions react to
events: state mutators call `uniflow::NotifyAll()`, which flips
`force_wake` and bypasses the wait gate on the same pump round.
Call sites of NotifyAll() (do not add more without need - these were
the four decision points that move the line forward):

- `GlobalEnv::CreateFakeZoneAPart()` -> wakes TryStartLoadPicker.
- `Stage::OnRawPartReceived()` -> wakes TryStartStageProcessing.
- `Stage::OnProcessedPartTaken()` -> wakes TryStartLoadPicker for the
  next round (Stage idle).
- `Stage::OnProcess_ReturnToPickPos` at the state -> ProcessedPartReady
  transition -> wakes TryStartUnloadPicker.

Flow termination (e.g. picker FLOW END) already auto-Notifies via
uniflow's own ClearFlow path - we do not need to add it manually for
picker IsIdle changes.

**Two-finger gripper viz DONE.** Each picker now has a `MotorAxis
finger_axis_` (initial pose = open). `OnLoad_HandGrip` /
`OnUnload_HandGrip` set target 0.0 and Stay() until InPosition;
`OnLoad_HandRelease` / `OnUnload_HandRelease` set target
`kFingerOpen_mm` and Stay() until InPosition. The Win32 painter draws
two short vertical bars at `hx +/- finger_gap_mm/2 * 1.5px` and, when
carrying, a yellow part rect between them. Console viz keeps the old
`v`/`#` markers.

**Smoke tests passed.** Console build (`UNIFLOW_CONSOLE_VIZ`) ran 20 s
and produced `delivered=1`. All flows ended `DONE`. Win32 build
compiled clean.

---

## Build commands

```powershell
$bat = "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
$src = "d:\clones\uniflow-cpp\examples\cnc_pickers"
$srcs = "main.cpp","globals.cpp","snapshot.cpp","picker_motion.cpp", `
        "stage.cpp","load_picker.cpp","unload_picker.cpp", `
        "orchestrator.cpp","env_log_observer.cpp","viz.cpp" |
        ForEach-Object { "`"$src\$_`"" }
$srcsJoined = $srcs -join " "

# Console (no Win32, runs ~20s and exits)
cmd /c "call `"$bat`" >nul && cl /nologo /std:c++17 /EHsc /W3 /utf-8 /DUNIFLOW_CONSOLE_VIZ /I `"$src`" /Fo`"d:\clones\uniflow-cpp\build\\`" /Fe:`"d:\clones\uniflow-cpp\build\cnc_pickers_console.exe`" $srcsJoined 2>&1"

# Win32 (real window, runs until closed)
if (-not (Test-Path 'd:\clones\uniflow-cpp\build\win')) { New-Item -ItemType Directory -Path 'd:\clones\uniflow-cpp\build\win' -Force | Out-Null }
cmd /c "call `"$bat`" >nul && cl /nologo /std:c++17 /EHsc /W3 /utf-8 /I `"$src`" /Fo`"d:\clones\uniflow-cpp\build\win\\`" /Fe:`"d:\clones\uniflow-cpp\build\cnc_pickers.exe`" $srcsJoined /link user32.lib gdi32.lib 2>&1"
```

Expected warning at uniflow.hpp ~2920 (`warning C4267` from
BS::thread_pool) - harmless, leave it.

---

## Library status (uniflow.hpp)

The library is a single header at `examples/cnc_pickers/uniflow.hpp`.
The root `include/uniflow.hpp` was renamed to `uniflow.hpp.bak` so the
example's own copy is unambiguously the source of truth. **Do not
promote the bak file back without first merging the example copy's
changes** - there are many.

What the library can do (the major additions are in the third column):

| Area | API | Notes |
|---|---|---|
| Module skeleton | `UF_USES_UNIFLOW(Cls)` / `UF_SINGLETON(Cls)` | unchanged |
| Step declaration | `StepResult OnSomething(...)` | unchanged |
| Step transitions | `UF_NEXT(fn, args...)` | variadic - args captured into the next step fn |
| Step result helpers | `Stay()`, `Wait(d)`, `Wait()` (default poll), `Done()`, `Fail()` | `Wait()` overload uses `Config::default_step_poll` |
| Description | `Describe(args...)` | ostream-style variadic; per-step description threaded through Observer |
| Async submission | `UF_ASYNC(fn, ...)`, `UF_ASYNC_OPT(fn, opts, ...)` | unchanged |
| Async retrieval | `AsyncResult<T>()` | unchanged |
| Polling helper | `UFTimer` with `Elapsed()`, `TimedOut(d)`, `OnWait(condition, timeout)` (callable AND bool overloads), `Restart()` | tutorial doc inline above the class |
| Module control | `StartFlow` (raw) and `StartFlowAt(file, line, ...)` + macro `UF_START_FLOW(mod, fn, args...)` | macro captures `__FILE__/__LINE__` |
| Caller blocking | `WaitUntilIdle()` (renamed from `Wait()` to avoid clash with static `Wait()` step helper) | |
| External wake | `uniflow::NotifyAll()` + per-runtime `Notify()` | wake every pump out of cv-wait; also flips per-round `force_wake` flag that bypasses per-module `Wait(d)` gates |
| Pump | cv-based (was `sleep_for` polling) | async worker, `StartFlow`, and `ClearFlow` all call `Notify()` automatically |
| Exception policy | `bool CatchStepExceptions() const` CRTP hook | default `false` -> rethrow (std::terminate via the pump thread); override -> log via `OnStepThrew` + Fail |
| Observer signature | `OnFlowStarted(obj, FlowOrigin)`, `OnStepRan(obj, step, description, step_ordinal, tick, elapsed_cpu)`, `OnStepThrew(...)`, `OnFlowEnded(..., final_step_ordinal, total_ticks, ..., FlowOrigin)` | `step_ordinal` increments only on Advance; `tick` increments every body invocation |
| Bundled pool | `BSThreadPoolExecutor` (`BS::thread_pool` v4.1.0 inlined at the bottom of uniflow.hpp with the MIT notice) | register via `uniflow::RegisterExecutor("default", std::make_shared<uniflow::BSThreadPoolExecutor>(N))` |

---

## ASCII-only invariant

**Hard rule for code and comments in this project:** plain ASCII only.
No smart quotes, no em-dash, no arrows, no box-drawing characters. The
user enforces this strictly (see existing memory). Korean comments are
the one exception - but in *this* example the goal is ASCII-only source.
If a Korean comment ends up in a file, it was a `#claude` marker (the
user's review tag) and should be removed in cleanup, not preserved.

If a file goes through the IDE while a `#claude` Korean comment is
present, the IDE may re-save it as cp949. Detection + fix:

```python
with open(path, 'rb') as f: raw = f.read()
try:    text = raw.decode('utf-8')
except UnicodeDecodeError:
        text = raw.decode('cp949')
with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write(text)
```

---

## User preferences and decisions

Distilled from memory + chat. Apply to all future work:

- **Responses in Korean.** Terse, no trailing wrap-up summaries.
- **Don't ask before doing.** Plan, decide a reasonable default,
  execute. Save AskUserQuestion for genuinely two-way decisions.
- **Domain instinct first.** The user is a senior factory-automation /
  motion-control engineer. Naming, step granularity, HW abstraction
  layering - match real-world plant code, not "abstract clean" code.
- **No `Maybe...` prefix.** Use `Try...` (e.g. `TryStartLoadPicker`).
- **No `Line...` / "공장 느낌" naming.** Use generic words
  (`LineState` -> `GlobalEnv`, `StartLine` -> `InitializeEnv`).
- **No `Spawn`.** Use `CreateFake` for simulated-creation routines.
- **No `thunk`.** Use `step_fn` (matches uniflow.hpp's `curr_fn_` /
  `next_fn`).
- **`ordinal` semantics:** Advance-only. A Stay/Wait re-entry of the
  same step keeps the same `step_ordinal_`; a separate `tick_count_`
  counts re-entries.
- **Log layout = fixed columns.** Designed for wide terminals.
  Column widths in `EnvLogObserver`: obj 14, step 28, desc 40.
- **Step poll cadence is global, not per-call.** `Config::default_
  step_poll = 20ms`. Steps call `Wait()` (no arg) by default.
  `Wait(d)` for explicit override only.
- **Picker motion model:** "command" step (issues SetTarget, advances
  immediately) + "wait at" step (polls InPosition with `Stay()` for
  fastest response). Gripper grip/release follows the same axis-driven
  pattern via `finger_axis_`.
- **HW does not know about uniflow.** `HwSimulator::DoReady()` must
  NOT call `uniflow::NotifyAll()`; modules poll. (The general API
  exists for cases where the HW adapter layer is also a uniflow module
  and DOES want to notify.)
- **Step exception policy:** default is rethrow (fail-fast,
  std::terminate). Each class can opt in to catch+log+Fail by
  overriding `CatchStepExceptions()`. cnc_pickers stays on the default.
- **Comments are sparing.** Prefer naming. When a comment exists, it
  explains *why*, not *what*. No multi-paragraph docstrings.

---

## Memory locations

- **Per-project memory (auto-loaded each session):**
  `C:\Users\windows10\.claude\projects\d--clones-uniflow-cpp\memory\`
  Contains: `user_role.md`, `feedback_ascii_only.md`,
  `project_handoff.md`, `MEMORY.md` (index).
- **Project notes:** `d:\clones\uniflow-cpp\notes\HANDOFF.md` (this
  file), `notes\make_concept.hpp`.

---

## Smoke-test expectations (after split + gripper viz)

Console build, `cnc_pickers_console.exe`, the log should look like:

```
[Viz           ] FLOW START  caller=...main.cpp:NN
[Orchestrator  ] FLOW START  caller=...main.cpp:NN
[LoadPicker    ] FLOW START  caller=...orchestrator.h:NN
[LoadPicker    ] OnLoad_CmdMoveToSource     cmd: move X to zone A (200 mm)   #01/#02 elapsed=0.01ms
[LoadPicker    ] OnLoad_WaitAtSource        approaching A: 200 mm            #02/#NN elapsed=0.0Xms
... (many Stay() ticks at #02 while the axis settles)
[LoadPicker    ] OnLoad_HandGrip            hand: closing gripper (gap=X mm) #NN/#NN elapsed=0.00ms
...
[Stage         ] ASYNC SUBMIT  SimulateStartCmd
[Stage         ] ASYNC DONE    SimulateStartCmd  wait=~1000ms
[Stage         ] OnProcess_WaitHwReady     hw ready handshake settling     #03/#NN
[Stage         ] FLOW END  DONE  steps=#07 ticks=#NNN  wall=~8000ms  cpu=~1.3ms  async=~1500ms
```

At least one full Stage cycle (steps=#07, sometimes #04 if it gets
preempted by Stop) should complete within a 20-second console run, and
`delivered>=1` at end. Verified delivered=1 on this session's run.

If a build error mentions `BS::thread_pool` not visible, the BS body
did not survive an edit - search the bottom of `uniflow.hpp` for
`namespace BS` and the inlined `class thread_pool`.
