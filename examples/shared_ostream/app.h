// app.h - one Runtime, two UF_Writer instances. Both writers share the
// same pump thread, which is exactly the trick: writes to the same
// ostringstream from "different modules" can be lock-free because the
// framework already serialises them.
#pragma once

#include "uf_writer.h"
#include "uniflow.hpp"

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
    UF_Writer        hello{rt, "Hello"};
    UF_Writer        world{rt, "World"};

    // Phase 2 - kick the flows off. Each writer is parameterised by
    // its text, its repeat count, and its turn id (0 or 1).
    void Start(int repeats)
    {
        UF_START_FLOW(hello, OnWrite_Begin, std::string("Hello "),  repeats, 0);
        UF_START_FLOW(world, OnWrite_Begin, std::string("World. "), repeats, 1);
    }

    void WaitForDone()
    {
        hello.WaitUntilIdle();
        world.WaitUntilIdle();
    }

private:
    App() = default;

    static uniflow::Runtime::Opts MakeOpts()
    {
        using namespace std::chrono_literals;
        uniflow::Runtime::Opts opts;
        opts.threads = 2;
        // Stay() comes back immediately - the two writers ping-pong via the
        // turn flag, so we want the pump to spin both modules tight.
        opts.config.step_interval_sleep_ms = 0ms;
        opts.config.stay_sleep_ms          = 0ms;
        opts.config.idle_sleep_ms          = 1ms;
        return opts;
    }
};
