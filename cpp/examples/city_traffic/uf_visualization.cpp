// uf_visualization.cpp - pump-side snapshot writer + render dispatcher.
// Platform-independent: <windows.h> lives only in uf_visualization_win32.cpp,
// ANSI/console code only in uf_visualization_console.cpp.
#define _CRT_SECURE_NO_WARNINGS // std::getenv is fine and portable here
#include "uf_visualization.h"

#include "globals.h"
#include "map.h"
#include "snapshot.h"
#include "world.h"

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

// ----- pump side: World -> Snapshot every tick -----
//
// This runs on the pump thread. It snapshots the shared World (signals +
// vehicle poses) under g_snap_mu so the render thread sees a consistent frame.
// That mutex is the ONLY cross-thread synchronisation in the demo.
uniflow::StepResult Flow_Visualization::Task_Snapshot::Step_Tick()
{
    if (sim::Stop())
        return Done();

    std::vector<city::SignalState> sigs;
    sigs.reserve(citymap::Nodes().size());
    for (const citymap::Node& n : citymap::Nodes())
        sigs.push_back(city::GetSignal(n.id));

    std::vector<VehicleView> cars;
    for (const city::VehicleState& v : city::Vehicles())
    {
        if (!v.active)
            continue;
        VehicleView vw;
        vw.gx    = v.gx; // centreline position published by the vehicle
        vw.gy    = v.gy;
        vw.dx    = v.dx;
        vw.dy    = v.dy;
        vw.blink = v.blink;
        vw.r     = v.r;
        vw.g     = v.g;
        vw.b     = v.b;
        cars.push_back(vw);
    }

    {
        std::lock_guard<std::mutex> lk(g_snap_mu);
        g_snap.signals  = std::move(sigs);
        g_snap.vehicles = std::move(cars);
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
