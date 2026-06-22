// globals.h - shared types plus a process-wide stop latch.
//
// FEATURE FOCUS: the sender and the receiver run as two modules on ONE pump
// thread, so the mailbox between them needs no lock at all. This header only
// holds the value type they exchange, the demo's tuning constants, and the
// cross-thread stop flag the stdin loop sets.
#pragma once

#include "uniflow.hpp"

#include <chrono>

using namespace std::chrono_literals;

// One arithmetic job: the sender pushes it, the receiver pops and evaluates it.
struct Msg
{
    int  a  = 0;
    int  b  = 0;
    char op = '+';
};

class GlobalConfig
{
public:
    static constexpr int               kVecSize       = 10;
    static constexpr int               kValueMax      = 30;
    static constexpr int               kBurstMin      = 1;
    static constexpr int               kBurstMax      = 10;
    static constexpr uniflow::Duration kSendGap       = 600ms; // gap between bursts
    static constexpr int               kMaxBurstCount = 20;    // stop the sender here

    GlobalConfig() = delete;
};

// Process-wide stop latch. The stdin loop sets it; every step polls it and
// returns Done(), which lets WaitUntilIdle() return at shutdown.
class GlobalEnv
{
public:
    static bool Stop();
    static void RequestStop();

    GlobalEnv() = delete;
};
