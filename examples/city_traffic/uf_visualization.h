// uf_visualization.h - copies World state into g_snap each tick (the pump
// side); RunVisualisation() renders on the main thread. <windows.h> is
// confined to the .cpp.
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
