// cnc_pickers - CNC line demo. LoadPicker A->B, Stage processes at B,
// UnloadPicker B->C. Pickers can never share zone B. All modules live
// as members of App (app.h); Orchestrator schedules.
#include "app.h"
#include "viz.h"

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
