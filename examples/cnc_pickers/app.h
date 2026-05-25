// app.h - holds the Runtime + every module. Two-phase init:
//   Phase 1 - members construct in declaration order. ctor bodies must
//             NOT touch App::inst().X (later members may not exist yet).
//   Phase 2 - Start() launches flows; cross-module access is now safe.
// Step bodies always run in phase 2 so App::inst().peer.X() is fine there.
#pragma once

#include "env_log_observer.h"
#include "load_picker.h"
#include "orchestrator.h"
#include "stage.h"
#include "uniflow.hpp"
#include "unload_picker.h"
#include "viz.h"

#include <memory>

class App
{
public:
    static App& inst()
    {
        static App a;
        return a;
    }

    // Phase 1.
    uniflow::Runtime rt{MakeOpts()};
    Stage         stage{rt};
    Viz           viz{rt};
    LoadPicker    load{rt};
    UnloadPicker  unload{rt};
    Orchestrator  orch{rt};

    // Phase 2.
    void Start()
    {
        UF_START_FLOW(viz,  OnViz_Begin);
        UF_START_FLOW(orch, OnSchedule_Begin);
    }

    void Shutdown()
    {
        GlobalEnv::RequestStop();
        orch  .WaitUntilIdle();
        load  .WaitUntilIdle();
        unload.WaitUntilIdle();
        stage .WaitUntilIdle();
        viz   .WaitUntilIdle();
    }

private:
    App() = default;

    static uniflow::Runtime::Opts MakeOpts()
    {
        uniflow::Runtime::Opts opts;
        opts.threads  = 8;
        opts.observer = std::make_unique<EnvLogObserver>("cnc_pickers.log");
        return opts;
    }
};
