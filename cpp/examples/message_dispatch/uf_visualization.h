// uf_visualization.h - one flow that copies prof / friend / mailbox /
// student state into g_snap every pump round. RunVisualisation() hosts
// the Win32 message loop on the main thread; it reads g_snap to paint.
#pragma once

#include "uniflow.hpp"

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

// Blocks the calling (main) thread until the user closes the window.
void RunVisualisation();
