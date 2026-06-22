// app.h - holds the Runtime + every module. Two-phase init:
//   Phase 1 - members construct in declaration order. ctor bodies must
//             NOT touch App::inst().X (later members may not exist yet).
//   Phase 2 - Start() launches flows; cross-module access is now safe.
// Step bodies always run in phase 2 so App::inst().peer.X() is fine there.
#pragma once

#include "env_log_observer.h"
#include "uniflow.hpp"
#include "uf_load_picker.h"
#include "uf_orchestrator.h"
#include "uf_stage.h"
#include "uf_unload_picker.h"
#include "uf_visualization.h"

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
    Flow_Stage         stage{rt};
    Flow_Visualization           viz{rt};
    Flow_LoadPicker    load{rt};
    Flow_UnloadPicker  unload{rt};
    Flow_Orchestrator  orch{rt};

    // Phase 2.
    void Start()
    {
        viz.ctx_snapshot_.StartFlow();
        orch.ctx_schedule_.StartFlow();
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

    // Silent observer for console mode: the ANSI renderer owns stdout, so the
    // EnvLogObserver (which logs to stdout + a file) must be suppressed there.
    struct SilentObserver : uniflow::IUniflowObserver
    {
    };

    static uniflow::Runtime::Opts MakeOpts()
    {
        using namespace std::chrono_literals;
        uniflow::Runtime::Opts opts;
        opts.threads  = 8;
        if (UseConsoleRenderer())
        {
            opts.observer = std::make_unique<SilentObserver>();
        }
        else
        {
            opts.observer = std::make_unique<EnvLogObserver>("pick_and_place.log");
        }
        // Pump sleep policy. Defaults are fine for this demo; shown here
        // so it is obvious where to tune them.
        opts.config.step_interval_sleep_ms = 0ms;  // any-Next round: burst
        opts.config.stay_sleep_ms          = 20ms; // active but all-Stay
        opts.config.idle_sleep_ms          = 1ms;  // no flows running
        return opts;
    }
};
