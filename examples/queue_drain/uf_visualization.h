// uf_visualization.h - one flow that copies the sender / receiver /
// mailbox state into g_snap every pump round; the Win32 paint loop
// in RunVisualisation() reads g_snap on the main thread.
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

void RunVisualisation();
