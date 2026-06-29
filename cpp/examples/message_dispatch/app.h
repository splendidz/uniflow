// app.h - the Runtime plus all four modules (professor / friend / student /
// visualization). Two-phase init mirrors the other reference examples:
//   Phase 1 - members construct in declaration order. ctor bodies must NOT
//             touch App::inst().X (later members may not exist yet).
//   Phase 2 - Start() launches the flows; cross-module access is now safe.
// The student is woken by a spawner's StartFlow(); it parks itself (Stay) when
// the mailbox empties and ends only when both spawners are done.
//
// 'friend' is a C++ keyword, so the member is named 'friend_'.
#pragma once

#include "uf_friend.h"
#include "uf_professor.h"
#include "uf_student.h"
#include "uf_visualization.h"
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

    // Phase 1: construct.
    uniflow::Runtime   rt{MakeOpts()};
    Flow_Professor     prof{rt};
    Flow_Friend        friend_{rt};
    Flow_Student       student{rt};
    Flow_Visualization viz{rt};

    // Phase 2: launch. The viz snapshot task and the two spawner tasks start
    // here; the student task is launched on demand by a spawner's first post.
    void Start()
    {
        viz.task_snapshot_.StartFlow();
        prof.task_emit_.StartFlow();
        friend_.task_emit_.StartFlow();
    }

    void Shutdown()
    {
        // Set after the user presses Enter; every step checks it and returns
        // Done(), so WaitUntilIdle() can return.
        GlobalEnv::RequestStop();
        rt.Wake();   // nudge the pump out of any sleep
        prof   .WaitUntilIdle();
        friend_.WaitUntilIdle();
        student.WaitUntilIdle();
        viz    .WaitUntilIdle();
    }

private:
    App() = default;

    // Silent observer: the ANSI renderer OWNS stdout, so the default
    // ConsoleObserver's step-trace output must be suppressed. IUniflowObserver's
    // hooks are empty by default, so an empty subclass prints nothing.
    struct SilentObserver : uniflow::IUniflowObserver
    {
    };

    static uniflow::Runtime::Opts MakeOpts()
    {
        using namespace std::chrono_literals;
        uniflow::Runtime::Opts opts;
        opts.threads  = 4;
        opts.observer = std::make_unique<SilentObserver>();
        opts.config.step_interval_sleep_ms = 0ms;
        opts.config.stay_sleep_ms          = 20ms;
        opts.config.idle_sleep_ms          = 1ms;
        return opts;
    }
};
