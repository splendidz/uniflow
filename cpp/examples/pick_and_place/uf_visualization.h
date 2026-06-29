// uf_visualization.h - snapshots line state into g_snap every kVizTick (pump
// side); RunVisualisation() renders on the main thread.
//
// DUAL RENDERER: the pump side (Step1_Tick) is platform-independent. The render
// side has two back-ends that read the same Snapshot:
//   - Win32 GDI window  (uf_visualization_win32.cpp)    -> Windows only
//   - ANSI console       (uf_visualization_console.cpp)  -> any terminal/Linux
// RunVisualisation() picks one via UseConsoleRenderer(): always console off
// Windows; on Windows the Win32 window unless UF_RENDER=console is set.
#pragma once

#include "globals.h"

class Flow_Visualization : public uniflow::Uniflow<Flow_Visualization>
{
public:
    explicit Flow_Visualization(uniflow::Runtime& rt);

    // The single perpetual Snapshot task (public so app.Start() launches it with
    // task.StartFlow()). Its one step copies line state into g_snap every tick
    // (perpetual poll; ends only on Stop).
    struct Task_Snapshot : uniflow::Task<Flow_Visualization>
    {
        StepResult Entry() override { return Step1_Tick(); }

    private:
        StepResult Step1_Tick();
    } task_snapshot_;
};

// True when the console back-end should be used (always off Windows; on Windows
// only when UF_RENDER=console is set).
bool UseConsoleRenderer();

// Main-thread entry: dispatches to the platform back-end.
void RunVisualisation();

// Back-ends (one is a no-op stub on the platform it does not apply to).
void RunVisualisationWin32();
void RunVisualisationConsole();
