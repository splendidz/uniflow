// tut2_async.cpp - the async quickstart from the README.
// A slow job is handed to the thread pool with UF_ASYNC; the result arrives
// in the next step. Meanwhile a heartbeat keeps ticking on the same pump,
// proving the 500ms job never blocked it.
//
// Build & run (portable, console only):
//   g++ -std=c++17 -O2 -pthread -I . examples/quickstart/tut2_async.cpp -o tut2 && ./tut2
//   cl /std:c++17 /EHsc /I . examples\quickstart\tut2_async.cpp /Fe:tut2.exe && tut2.exe
#include "uniflow.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

struct Silent : uniflow::IUniflowObserver
{
};

// A module that offloads a slow computation, then uses the result.
class Worker : public uniflow::Uniflow<Worker>
{
    UF_UNIFLOW_IMPLEMENT(Worker);

public:
    explicit Worker(uniflow::Runtime& rt) : uniflow::Uniflow<Worker>(rt) {}

    StepResult OnWork_Begin()
    {
        std::cout << "[worker] submitting slow job (pump is NOT blocked)\n";
        UF_ASYNC(SlowSquare, 9);     // runs on a pool thread
        return UF_NEXT(OnWork_Done); // result arrives in the next step
    }

private:
    StepResult OnWork_Done()
    {
        auto r = AsyncResult<int>();
        if (r.failed())
            return Fail();
        std::cout << "[worker] result in: 9 * 9 = " << r.value() << "\n";
        return Done();
    }

    // A UF_ASYNC target must be static - it runs on another thread, so it
    // cannot touch instance members (enforced at compile time). Inputs are
    // copied in as arguments.
    static int SlowSquare(int n)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(500)); // pretend slow
        return n * n;
    }
};

// A heartbeat on the same pump - proof the 500ms job never blocked it.
class Heartbeat : public uniflow::Uniflow<Heartbeat>
{
    UF_UNIFLOW_IMPLEMENT(Heartbeat);

public:
    explicit Heartbeat(uniflow::Runtime& rt) : uniflow::Uniflow<Heartbeat>(rt) {}

    StepResult OnBeat_Tick()
    {
        if (beats_ >= 5)
            return Done();
        auto now = uniflow::Clock::now();
        if (now - last_ >= std::chrono::milliseconds(100))
        {
            last_ = now;
            std::cout << "        [heartbeat] still ticking (" << ++beats_ << ")\n";
        }
        return Stay(); // re-run next round; pump naps in between
    }

private:
    int                beats_ = 0;
    uniflow::TimePoint last_  = uniflow::Clock::now();
};

int main()
{
    uniflow::Runtime::Opts opts;
    opts.observer = std::make_unique<Silent>();
    uniflow::Runtime rt{std::move(opts)};

    Worker    w{rt};
    Heartbeat h{rt};

    UF_START_FLOW(w, OnWork_Begin);
    UF_START_FLOW(h, OnBeat_Tick);

    w.WaitUntilIdle();
    h.WaitUntilIdle();
    return 0;
}
