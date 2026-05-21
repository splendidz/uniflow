// ============================================================================
//  03_polling.cpp — Stay(): re-running a step until a condition holds
//
//  Teaches: the Stay() result. A step that returns Stay() is invoked again
//           on the next pump tick *without advancing*. This is how you wait
//           for something cooperatively: the step yields control after every
//           check, so other modules keep running on the same thread.
//
//  Here a step polls until a dependency reports ready (think: waiting on a
//  service health check, or for a warm-up to finish).
//
//  When to use Stay() vs async: Stay() re-runs as fast as the pump cycles,
//  so use it only for cheap, non-blocking status checks. For a genuinely
//  long or blocking wait, offload it to the pool instead — see 02_async.cpp.
//
//  Build (from repo root):
//    cl /std:c++17 /EHsc /I include ^
//       examples\tutorials\03_polling.cpp /Fe:build\03_polling.exe
//    g++ -std=c++17 -pthread -I include ^
//       examples/tutorials/03_polling.cpp -o build/03_polling
// ============================================================================
#include "uniflow.hpp"

#include <chrono>
#include <iostream>
#include <thread>

class ReadyProbe : public uniflow::Uniflow<ReadyProbe>
{
    using S = ReadyProbe;
    UF_USES_UNIFLOW(ReadyProbe);

public:
    explicit ReadyProbe(uniflow::UniflowRuntime& rt)
        : Uniflow(rt)
    {
        UF_ENTRY("Probe", OnProbe_Begin);
    }

private:
    StepResult OnProbe_Begin()
    {
        std::cout << "  waiting for the dependency to become ready\n";
        polls_ = 0;
        return UF_NEXT(OnProbe_WaitReady);
    }

    StepResult OnProbe_WaitReady()
    {
        ++polls_;
        // A real check would query a health endpoint or a status flag. We
        // fake it: the dependency "reports ready" only on the 3rd poll.
        const bool ready = (polls_ >= 3);

        if (!ready)
        {
            std::cout << "  poll " << polls_ << ": not ready yet\n";
            return Stay(); // run THIS step again next tick — do not advance
        }

        std::cout << "  poll " << polls_ << ": ready\n";
        return UF_NEXT(OnProbe_Done);
    }

    StepResult OnProbe_Done()
    {
        std::cout << "  dependency is up - proceeding\n";
        return Done();
    }

    int polls_ = 0;
};

int main()
{
    uniflow::UniflowRuntime rt;
    auto*                   probe = rt.Create<ReadyProbe>("probe");

    rt.RunInBackground();

    probe->Start("Probe");
    while (!probe->IsIdle())
        std::this_thread::sleep_for(std::chrono::milliseconds(2));

    return 0;
}
