// uf_visualization.cpp - pump-side snapshot writer + render dispatcher.
// Platform-independent: <windows.h> lives only in uf_visualization_win32.cpp,
// ANSI/console code only in uf_visualization_console.cpp.
#define _CRT_SECURE_NO_WARNINGS // std::getenv is fine and portable here
#include "uf_visualization.h"

#include "app.h"
#include "globals.h"
#include "snapshot.h"
#include "uf_load_picker.h"
#include "uf_stage.h"
#include "uf_unload_picker.h"

#include <cstdlib>
#include <cstring>
#include <mutex>

using namespace uniflow;

Flow_Visualization::Flow_Visualization(uniflow::Runtime& rt)
    : uniflow::Uniflow<Flow_Visualization>(rt, "Flow_Visualization")
{
    AddTask(task_snapshot_);
}

// Pump-side: copy live line state into g_snap under g_snap_mu every tick, so
// the render thread sees a consistent frame. That mutex is the only cross-
// thread synchronisation in the demo.
StepResult Flow_Visualization::Task_Snapshot::Step1_Tick()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    const auto& load   = App::inst().load;
    const auto& unload = App::inst().unload;
    const auto& stage  = App::inst().stage;
    {
        std::lock_guard<std::mutex> lk(g_snap_mu);
        g_snap.load_x_mm            = load.X_mm();
        g_snap.load_z_mm            = load.Z_mm();
        g_snap.load_carry           = load.Carrying();
        g_snap.load_finger_gap_mm   = load.FingerGap_mm();
        g_snap.load_phase           = load.CurrentStepDescription();
        g_snap.unload_x_mm          = unload.X_mm();
        g_snap.unload_z_mm          = unload.Z_mm();
        g_snap.unload_carry         = unload.Carrying();
        g_snap.unload_finger_gap_mm = unload.FingerGap_mm();
        g_snap.unload_phase         = unload.CurrentStepDescription();
        g_snap.stage_table_x_mm     = stage.TableX_mm();
        g_snap.stage_table_y_mm     = stage.TableY_mm();
        g_snap.stage_state          = stage.state();
        g_snap.stage_phase          = stage.CurrentStepDescription();
        g_snap.zoneA_has_part       = GlobalEnv::ZoneAHasPart();
        g_snap.delivered            = GlobalEnv::DeliveredCount();
    }
    return Stay();
}

// ----- render-side dispatch -----

bool UseConsoleRenderer()
{
#if !defined(_WIN32)
    return true; // no Win32 GUI off Windows
#else
    const char* e = std::getenv("UF_RENDER");
    return e != nullptr && std::strcmp(e, "console") == 0;
#endif
}

void RunVisualisation()
{
    if (UseConsoleRenderer())
        RunVisualisationConsole();
    else
        RunVisualisationWin32();
}
