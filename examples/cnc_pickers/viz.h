// ======================================================================
//  viz.h - Viz module + the visualisation main loop entry point.
//
//  Viz is a uniflow singleton that snapshots the whole line state into
//  g_snap every kVizTick; the rendering loop (Win32 window on Windows,
//  console animation elsewhere) runs on the main thread and reads
//  through ReadSnapshot().
//
//  Step bodies and RunVisualisation()/Win32 paint code live in viz.cpp
//  so <windows.h> does not leak into any other translation unit.
// ======================================================================
#pragma once

#include "globals.h"

class Viz : public uniflow::Uniflow<Viz>
{
    UF_SINGLETON(Viz);

public:
    StepResult OnViz_Begin();

private:
    StepResult OnViz_Tick();
};

void RunVisualisation();
