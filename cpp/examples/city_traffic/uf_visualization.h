// uf_visualization.h - copies World state into g_snap each tick (the pump
// side); RunVisualisation() renders on the main thread.
//
// DUAL RENDERER: the pump side (Step_Tick) is platform-independent. The render
// side has two back-ends that read the same Snapshot:
//   - Win32 GDI window  (uf_visualization_win32.cpp)    -> Windows only
//   - ANSI console       (uf_visualization_console.cpp)  -> any terminal/Linux
// RunVisualisation() picks one via UseConsoleRenderer(): always console off
// Windows; on Windows the Win32 window unless UF_RENDER=console is set.
#pragma once

#include "uniflow.hpp"

class Flow_Visualization : public uniflow::Uniflow<Flow_Visualization>
{
public:
    explicit Flow_Visualization(uniflow::Runtime& rt)
        : uniflow::Uniflow<Flow_Visualization>(rt)
    {
        AddTask(ctx_);
    }

    // The single perpetual Snapshot task: copy World -> g_snap every tick.
    struct Task_Snapshot : uniflow::Task<Flow_Visualization>
    {
        StepResult Entry() override { return Step_Tick(); }

    private:
        StepResult Step_Tick();
    } ctx_;
};

// True when the console back-end should be used (always off Windows; on Windows
// only when the UF_RENDER=console environment variable is set). app.h reads
// this to pick a silent observer for console mode (the console owns stdout).
bool UseConsoleRenderer();

// Main-thread entry: dispatches to the platform back-end.
void RunVisualisation();

// Back-ends (one is a no-op stub on the platform it does not apply to).
void RunVisualisationWin32();
void RunVisualisationConsole();
