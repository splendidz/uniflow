// shared_ostream - two Flow_Writer modules append to ONE ostringstream at the
// same time. They appear to race, but the Runtime drives every step on a single
// pump thread, so the shared sink stays consistent without a mutex.
//
// Each writer takes (text, count, turn_id). One writes "Hello " ten times, the
// other writes "World. " ten times. A shared turn flag forces them to alternate
// so the output reads:
//   "Hello World. Hello World. ..."  (x10)
//
// At the end we count how many times the substring "Hello World." appears - it
// must be exactly the configured repeat count. This is a finite program: it
// runs the writers, prints the interleaved-yet-ordered output, then exits.
//
// FEATURE FOCUS: lock-free shared state on one pump thread.
#include "app.h"
#include "shared_state.h"

#include <iostream>
#include <string>

namespace
{
    int CountOccurrences(const std::string& hay, const std::string& needle)
    {
        if (needle.empty())
        {
            return 0;
        }
        int         hits = 0;
        std::size_t pos  = 0;
        while ((pos = hay.find(needle, pos)) != std::string::npos)
        {
            ++hits;
            pos += needle.size();
        }
        return hits;
    }
}

int main()
{
    std::cout << "=== shared_ostream: two writers, one log, no locks ==="
              << "\n\n";

    App& app = App::inst();
    app.Start();
    app.WaitForDone();

    const std::string out = SharedState::Log().str();
    const int         got = CountOccurrences(out, "Hello World.");

    std::cout << "--- output ---\n" << out << "\n--- end ---\n";
    std::cout << "\nexpected \"Hello World.\" occurrences = " << App::kRepeats
              << ", got = " << got << "\n";
    if (got == App::kRepeats)
    {
        std::cout << "PASS: shared log is race-free, order preserved\n";
    }
    else
    {
        std::cout << "FAIL: order was not preserved\n";
    }

    return got == App::kRepeats ? 0 : 1;
}
