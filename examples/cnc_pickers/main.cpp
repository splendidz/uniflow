// ======================================================================
//  cnc_pickers - demo_concept.md problem 2, solved with uniflow.
//
//  CNC-style line:
//    - Raw parts appear at zone A (Load) at random intervals (1-5 s).
//    - LoadPicker carries a raw part A -> B.
//    - Stage (machining HW at zone B) talks to fake HW, waits for ready,
//      processes for ~5 s, then runs a cleanup handshake and returns its
//      table to the pick position.
//    - UnloadPicker carries the finished part B -> C (Unload).
//
//  Two pickers must never share the B zone - one must clear B (by at
//  least the safety gap) before the other enters.
//
//  Architecture (what this example teaches):
//    - LoadPicker and UnloadPicker are SEPARATE classes. They share
//      helpers but their job, source/dest, and gating conditions differ
//      enough that forcing them into one class hides intent.
//    - Each class lives in its own header so the example reads like a
//      real multi-module project: globals.h, stage.h, picker_motion.h,
//      load_picker.h, unload_picker.h, orchestrator.h, viz.h, etc. This
//      file contains only main() and the env lifecycle wiring.
//    - The Orchestrator is a singleton module that owns line-level
//      scheduling. Pickers and Stage do NOT decide what to do next -
//      they execute the orchestrator's command. They expose Ready/Idle
//      queries and accept commands via plain method calls (same pump
//      thread, no lock needed).
//    - Stage is a sequence of distinct steps (SendStart, WaitReady,
//      Processing, Cleanup, MoveToPickPos). Timing is wall-clock, never
//      frame-count. SendStart and Cleanup fan out to the thread pool
//      via UF_ASYNC to simulate a real HW handshake.
//    - Motion uses MotorAxis helpers exposing InPosition(), which only
//      returns true after the axis has held the target within tolerance
//      for a settling time. The two-finger gripper is a third MotorAxis
//      on each picker - SetTarget(0) closes, SetTarget(kFingerOpen_mm)
//      opens; InPosition gates the advance.
//
//  Visualisation: Win32 window on Windows, console animation elsewhere.
//
//  Build (from the repo root):
//    cl /std:c++17 /EHsc /utf-8 /I examples\cnc_pickers ^
//       examples\cnc_pickers\main.cpp examples\cnc_pickers\viz.cpp ^
//       /Fe:build\cnc_pickers.exe /link user32.lib gdi32.lib
// ======================================================================
#include "env_log_observer.h"
#include "globals.h"
#include "load_picker.h"
#include "orchestrator.h"
#include "snapshot.h"
#include "stage.h"
#include "unload_picker.h"
#include "viz.h"

#include <iostream>
#include <memory>

static void InitializeEnv()
{
    Stage::inst();
    Viz::inst();
    Orchestrator::inst();

    UF_START_FLOW(Viz::inst(),          OnViz_Begin);
    UF_START_FLOW(Orchestrator::inst(), OnSchedule_Begin);
}

static void ShutdownEnv()
{
    GlobalEnv::RequestStop();
    Orchestrator::inst().WaitUntilIdle();
    LoadPicker::GetInst().WaitUntilIdle();
    UnloadPicker::GetInst().WaitUntilIdle();
    Stage::inst().WaitUntilIdle();
    Viz::inst().WaitUntilIdle();
}

int main()
{
    // BS thread pool for the HW handshake simulations.
    uniflow::RegisterExecutor(
        "default", std::make_shared<uniflow::BSThreadPoolExecutor>(2));

    uniflow::SetObserver(std::make_unique<EnvLogObserver>("cnc_pickers.log"));

    // The two pickers are non-singleton modules - construct them here
    // so their names land in the registry before any flow looks them up.
    LoadPicker   load;
    UnloadPicker unload;

    InitializeEnv();
    RunVisualisation();
    ShutdownEnv();

    std::cout << "parts delivered to Unload: "
              << GlobalEnv::DeliveredCount() << "\n";
    return 0;
}
