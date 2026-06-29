// app.h - the Runtime plus every module. Two-phase init, same shape as the
// other reference examples: members construct first (Phase 1), then Start()
// launches the flows (Phase 2).
//
// REFERENCE NOTE: one Runtime, one pump thread, six flows (five runners + the
// renderer). They cooperate without a single lock between them.
#pragma once

#include "uf_runner.h"
#include "uf_view.h"
#include "uniflow.hpp"

#include <memory>

// Silent observer: this app OWNS the console (the dashboard), so the default
// ConsoleObserver's trace output must be suppressed. IUniflowObserver's hooks
// are all empty by default, so an empty subclass prints nothing.
struct SilentObserver : uniflow::IUniflowObserver
{
};

class App
{
public:
    static App& inst()
    {
        static App a;
        return a;
    }

    // Phase 1: construct. Order is renderer then runners; ctor bodies only touch
    // their own state, so cross-module order does not matter here.
    uniflow::Runtime rt{MakeOpts()};
    Flow_View        view{rt};
    Flow_Runner      atlas{rt, "Atlas", 0, 3800.0};
    Flow_Runner      bolt{rt, "Bolt", 1, 2600.0};
    Flow_Runner      comet{rt, "Comet", 2, 4600.0};
    Flow_Runner      dash{rt, "Dash", 3, 3200.0};
    Flow_Runner      echo{rt, "Echo", 4, 5200.0};

    // Phase 2: launch every task. Each StartFlow() puts one task on the pump.
    void Start()
    {
        view.task_draw_.StartFlow();
        atlas.task_run_.StartFlow();
        bolt.task_run_.StartFlow();
        comet.task_run_.StartFlow();
        dash.task_run_.StartFlow();
        echo.task_run_.StartFlow();
    }

    void Shutdown()
    {
        sim::g_stop.store(true);   // every step checks this and returns Done()
        rt.Wake();                 // nudge the pump out of any sleep
        atlas.WaitUntilIdle();
        bolt.WaitUntilIdle();
        comet.WaitUntilIdle();
        dash.WaitUntilIdle();
        echo.WaitUntilIdle();
        view.WaitUntilIdle();
    }

private:
    App() = default;

    static uniflow::Runtime::Opts MakeOpts()
    {
        using namespace std::chrono_literals;
        uniflow::Runtime::Opts opts;
        opts.observer = std::make_unique<SilentObserver>();
        // All-Stay rounds (everyone polling) should be short so motion and the
        // ~30 fps renderer stay smooth.
        opts.config.step_interval_sleep_ms = 0ms;
        opts.config.stay_sleep_ms          = 5ms;
        opts.config.idle_sleep_ms          = 1ms;
        return opts;
    }
};
