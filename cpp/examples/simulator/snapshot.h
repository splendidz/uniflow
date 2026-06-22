// snapshot.h - the data the renderer draws each frame.
//
// REFERENCE NOTE: there is NO mutex here, on purpose. Every Flow_Runner and the
// Flow_View renderer are modules on the SAME Runtime, so they all advance on the
// one pump thread, round-robin. A runner writing its row and the view reading it
// never overlap - that is the core uniflow guarantee: single thread, no locks.
// The only other thread (stdin in main) touches just the clock and g_stop, never
// these rows.
#pragma once

#include <atomic>
#include <string>

namespace sim
{

constexpr int kRunnerCount = 5;

// Fixed dashboard layout (1-based rows). The renderer only ever draws rows
// 1..(kPromptRow-1); the stdin prompt lives on kPromptRow and is never touched
// by the renderer, so typed input is not clobbered by frame redraws.
constexpr int kPromptRow = 6 + kRunnerCount;

// One dashboard line. The runner owns its slot (indexed by ctor id).
struct RunnerRow
{
    std::string name;
    std::string step    = "-";   // current step name - drives the "what is it doing" column
    double      percent = 0.0;
    int         lap     = 0;
};

// Plain global state, pump-thread-only (see header note above).
extern RunnerRow g_rows[kRunnerCount];

// Cross-thread shutdown latch: set by the stdin loop, read by every flow's
// steps so they can return Done() and let WaitUntilIdle() return.
extern std::atomic<bool> g_stop;

}  // namespace sim
