// globals.h - shared types and a simple line-level stop flag.
#pragma once

#include "uniflow.hpp"

#include <chrono>

using namespace std::chrono_literals;

// One arithmetic job pushed by the sender, popped by the receiver.
struct Msg
{
    int  a  = 0;
    int  b  = 0;
    char op = '+';
};

class GlobalConfig
{
public:
    static constexpr int                kVecSize       = 10;
    static constexpr int                kValueMax      = 30;
    static constexpr int                kBurstMin      = 1;
    static constexpr int                kBurstMax      = 10;
    static constexpr uniflow::Duration  kSendGap       = 1000ms;
    static constexpr uniflow::Duration  kVizTick       = 16ms;
    static constexpr int                kMaxBurstCount = 20; // when to stop the sender

    GlobalConfig() = delete;
};

class GlobalEnv
{
public:
    static bool Stop();
    static void RequestStop();

    GlobalEnv() = delete;
};
