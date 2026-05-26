// viz.h - snapshots line state into g_snap every kVizTick.
// RunVisualisation() (Win32 or console) renders on the main thread.
// <windows.h> is confined to viz.cpp.
#pragma once

#include "globals.h"

class Viz : public uniflow::Uniflow<Viz>
{
    UF_UNIFLOW_IMPLEMENT(Viz);

public:
    explicit Viz(uniflow::Runtime& rt)
        : uniflow::Uniflow<Viz>(rt) {}

    StepResult OnViz_Begin();

private:
    StepResult OnViz_Tick();
};

void RunVisualisation();
