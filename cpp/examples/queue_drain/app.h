// app.h - holds the Runtime, the sender, the receiver, the visualiser.
// Sender + receiver share the pump thread - the mailbox can be touched
// without locks.
#pragma once

#include "uf_receiver.h"
#include "uf_sender.h"
#include "uf_visualization.h"
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
    UF_Sender        send{rt};
    UF_Receiver      recv{rt};
    UF_Visualization viz{rt};

    void Start()
    {
        UF_START_FLOW(viz,  OnViz_Begin);
        UF_START_FLOW(send, OnSend_Begin);
    }

    void Shutdown()
    {
        GlobalEnv::RequestStop();
        send.WaitUntilIdle();
        recv.WaitUntilIdle();
        viz .WaitUntilIdle();
    }

private:
    App() = default;

    static uniflow::Runtime::Opts MakeOpts()
    {
        using namespace std::chrono_literals;
        uniflow::Runtime::Opts opts;
        opts.threads = 4;
        opts.config.step_interval_sleep_ms = 0ms;
        opts.config.stay_sleep_ms          = 20ms;
        opts.config.idle_sleep_ms          = 1ms;
        return opts;
    }
};
