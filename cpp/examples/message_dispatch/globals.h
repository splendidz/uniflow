// globals.h - shared types and timing for the professor / student / friend
// trio. All three modules live on the same Runtime, so the mailbox is
// touched on a single pump thread and needs no locking.
#pragma once

#include "uniflow.hpp"

#include <chrono>
#include <string>

using namespace std::chrono_literals;

struct Message
{
    enum class Kind { Assignment, Play };
    Kind        kind          = Kind::Assignment;
    std::string name;
    int         need_ability  = 0; // Assignment: required ability to do it
    int         need_time     = 0; // Assignment: hours of actual work
    int         play_hours    = 0; // Play:       hours of playing
};

class GlobalConfig
{
public:
    // One simulated hour on the pool. Keeps the demo finite.
    static constexpr int                kHourMs       = 8;
    // At or above this stress, the student must sleep before training.
    static constexpr int                kStressMax    = 10;

    static constexpr uniflow::Duration  kProfMinGap   = 200ms;
    static constexpr uniflow::Duration  kProfMaxGap   = 800ms;
    static constexpr uniflow::Duration  kFriendMinGap = 350ms;
    static constexpr uniflow::Duration  kFriendMaxGap = 1100ms;

    GlobalConfig() = delete;
};

// Window-close signal: main calls RequestStop() after the Win32 message
// loop returns, and the viz step checks Stop() so its flow can Done().
// Worker modules (prof / friend / student) don't read this - they end
// naturally when their lists / the mailbox drain.
class GlobalEnv
{
public:
    static bool Stop();
    static void RequestStop();

    GlobalEnv() = delete;
};
