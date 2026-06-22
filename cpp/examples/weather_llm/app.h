// app.h - one Runtime, one Uniflow module. The pump thread stays
// responsive while the two blocking calls (HTTPS GET, Gemini POST)
// happen on the pool.
#pragma once

#include "uf_weather.h"
#include "uniflow.hpp"

class App
{
public:
    static App& inst()
    {
        static App a;
        return a;
    }

    uniflow::Runtime rt{MakeOpts()};
    Flow_Weather     weather{rt};

    void Start()
    {
        // Launch the fetch task; the pump drives it to completion.
        weather.ctx_fetch_.StartFlow();
    }

    void Shutdown()
    {
        weather.WaitUntilIdle();
    }

private:
    App() = default;

    static uniflow::Runtime::Opts MakeOpts()
    {
        using namespace std::chrono_literals;
        uniflow::Runtime::Opts opts;
        opts.threads                       = 4;
        opts.config.step_interval_sleep_ms = 0ms;
        opts.config.stay_sleep_ms          = 50ms;
        opts.config.idle_sleep_ms          = 1ms;
        // Network calls can sit on the pool a long time; bump the
        // slow-async alarm threshold so we get one warning per stuck
        // call without spamming.
        opts.config.slow_async_threshold_ms = 5000ms;
        return opts;
    }
};
