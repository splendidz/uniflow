// pick_and_place - CNC line demo. Flow_LoadPicker A->B, Flow_Stage processes at B,
// Flow_UnloadPicker B->C. Pickers can never share zone B. All modules live
// as members of App (app.h); Flow_Orchestrator schedules.
#include "app.h"
#include "uf_visualization.h"

#include <iostream>

int main()
{
    App& app = App::inst();   // Phase 1: every module is now constructed.
    app.Start();              // Phase 2: flows start; cross-module refs safe.

    RunVisualisation();       // main-thread render loop (Win32 / console).

    app.Shutdown();

    std::cout << "parts delivered to Unload: "
              << GlobalEnv::DeliveredCount() << "\n";
    return 0;
}
