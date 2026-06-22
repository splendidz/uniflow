// app.h - one Runtime, two Flow_Writer instances. Both writers share the same
// pump thread, which is exactly the trick: writes to the same ostringstream
// from "different modules" can be lock-free because the framework already
// serialises every step on that one thread.
//
// Two-phase init: members construct (Phase 1), then Start() launches the tasks
// (Phase 2). Each writer is parameterised by its text, repeat count, and turn
// id (0 or 1) at construction.
#pragma once

#include "uf_writer.h"
#include "uniflow.hpp"

#include <memory>

class App
{
public:
    static App& inst()
    {
        static App a;
        return a;
    }

    // Phase 1 - construct the runtime and both writers.
    uniflow::Runtime rt{MakeOpts()};
    Flow_Writer      hello{rt, "Hello ", kRepeats, 0};
    Flow_Writer      world{rt, "World. ", kRepeats, 1};

    // Phase 2 - launch each writer's task on the pump.
    void Start()
    {
        hello.ctx_write_.StartFlow();
        world.ctx_write_.StartFlow();
    }

    void WaitForDone()
    {
        hello.WaitUntilIdle();
        world.WaitUntilIdle();
    }

    static constexpr int kRepeats = 10;

private:
    App() = default;

    // The verification owns stdout; suppress the default ConsoleObserver's trace
    // so only the program's own output appears. IUniflowObserver's hooks are all
    // empty by default, so an empty subclass prints nothing.
    struct SilentObserver : uniflow::IUniflowObserver
    {
    };

    static uniflow::Runtime::Opts MakeOpts()
    {
        using namespace std::chrono_literals;
        uniflow::Runtime::Opts opts;
        opts.observer = std::make_unique<SilentObserver>();
        // Stay() comes back immediately - the two writers ping-pong via the turn
        // flag, so spin both modules tight.
        opts.config.step_interval_sleep_ms = 0ms;
        opts.config.stay_sleep_ms          = 0ms;
        opts.config.idle_sleep_ms          = 1ms;
        return opts;
    }
};
