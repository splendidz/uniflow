// app.h - the Runtime plus all four modules (prof / friend / student /
// viz). Two-phase init mirrors cnc_pickers: members construct, then
// Start() arms the spawner flows and the viz flow. Student is woken by
// a spawner Post; it parks itself when the mailbox empties and both
// spawners are done.
//
// 'friend' is a C++ keyword, so the member is named 'friend_'.
#pragma once

#include "uf_friend.h"
#include "uf_professor.h"
#include "uf_student.h"
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
    UF_Professor     prof{rt};
    UF_Friend        friend_{rt};
    UF_Student       student{rt};
    UF_Visualization viz{rt};

    void Start()
    {
        UF_START_FLOW(viz,     OnViz_Begin);
        UF_START_FLOW(prof,    OnProf_Begin);
        UF_START_FLOW(friend_, OnFriend_Begin);
    }

    void Shutdown()
    {
        // After the Win32 message loop returns, this signals the viz
        // flow to Done. Worker modules end naturally.
        GlobalEnv::RequestStop();
        prof   .WaitUntilIdle();
        friend_.WaitUntilIdle();
        student.WaitUntilIdle();
        viz    .WaitUntilIdle();
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
