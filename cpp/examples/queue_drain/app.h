// app.h - the Runtime plus every module. Two-phase init, same shape as the
// other reference examples: members construct first (Phase 1), then Start()
// launches the flows (Phase 2).
//
// REFERENCE NOTE: one Runtime, one pump thread, three flows (sender, receiver,
// renderer). They cooperate without a single lock on the mailbox between them -
// the only mutex in the demo guards the render-thread snapshot.
#pragma once

#include "globals.h"
#include "uf_receiver.h"
#include "uf_sender.h"
#include "uf_visualization.h"

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

    // Phase 1: members construct in declaration order. ctor bodies touch only
    // their own state, so cross-module order does not matter here.
    uniflow::Runtime   rt{MakeOpts()};
    Flow_Sender        send{rt};
    Flow_Receiver      recv{rt};
    Flow_Visualization viz{rt};

    // Phase 2: launch the perpetual tasks. The receiver is NOT started here -
    // the sender relaunches its drain task on the first burst.
    void Start()
    {
        viz.task_snapshot_.StartFlow();
        send.task_emit_.StartFlow();
    }

    void Shutdown()
    {
        GlobalEnv::RequestStop();   // every step polls this and returns Done()
        rt.Wake();                  // nudge the pump out of any sleep
        send.WaitUntilIdle();
        recv.WaitUntilIdle();
        viz.WaitUntilIdle();
    }

private:
    App() = default;

    static uniflow::Runtime::Opts MakeOpts()
    {
        using namespace std::chrono_literals;
        uniflow::Runtime::Opts opts;
        opts.observer = std::make_unique<SilentObserver>();
        opts.config.step_interval_sleep_ms = 0ms;   // any-Next round: burst
        opts.config.stay_sleep_ms          = 20ms;  // active but all-Stay
        opts.config.idle_sleep_ms          = 1ms;   // no flows running
        return opts;
    }
};
