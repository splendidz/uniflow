// viz.h - snapshots line state into g_snap every kVizTick.
// RunVisualisation() (Win32 or console) renders on the main thread.
// <windows.h> is confined to viz.cpp.
#pragma once

#include "globals.h"

class UF_Visualization : public uniflow::Uniflow<UF_Visualization>
{
    UF_UNIFLOW_IMPLEMENT(UF_Visualization);

public:
    explicit UF_Visualization(uniflow::Runtime& rt)
        : uniflow::Uniflow<UF_Visualization>(rt) {}

    StepResult OnViz_Begin();

private:
    StepResult OnViz_Tick();
};

void RunVisualisation();
