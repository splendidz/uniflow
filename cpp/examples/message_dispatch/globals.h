// globals.h - shared types, timing, and a small activity log for the
// professor / student / friend trio. All three modules live on the same
// Runtime, so the mailbox is touched on a single pump thread and needs no
// locking. The activity log (recent professor/friend/student lines) is read
// by the render thread, so it carries its own mutex.
#pragma once

#include "uniflow.hpp"

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

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

// Quit signal: main sets this after the user presses Enter, and every flow's
// steps check it so they can return Done() and let WaitUntilIdle() return.
class GlobalEnv
{
public:
    static bool Stop();
    static void RequestStop();

    GlobalEnv() = delete;
};

// Recent-activity log. The pump-thread steps append lines (the old stdout
// prints would corrupt the ANSI dashboard); the render thread reads the last
// few for the "recent activity" panel. Hence the mutex.
class GlobalLog
{
public:
    static void                     Add(const std::string& line);
    static std::vector<std::string> Recent(std::size_t n);

    GlobalLog() = delete;
};
