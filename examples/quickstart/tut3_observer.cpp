// tut3_observer.cpp - the observability quickstart from the README.
// With the default ConsoleObserver, the whole flow (step transitions, async
// start/finish, per-step time, flow summary) is logged with no trace code.
// A custom observer (Metrics) shows how to feed your own metrics/alerts.
//
// Build & run (portable, console only):
//   g++ -std=c++17 -O2 -pthread -I . examples/quickstart/tut3_observer.cpp -o tut3 && ./tut3
//   cl /std:c++17 /EHsc /I . examples\quickstart\tut3_observer.cpp /Fe:tut3.exe && tut3.exe
#include "uniflow.hpp"

#include <chrono>
#include <memory>
#include <string_view>
#include <thread>
#include <vector>

// fetch -> parse -> store. Not a single line of measurement code.
class Pipe : public uniflow::Uniflow<Pipe>
{
    UF_UNIFLOW_IMPLEMENT(Pipe);

public:
    explicit Pipe(uniflow::Runtime& rt) : uniflow::Uniflow<Pipe>(rt) {}

    StepResult OnPipe_Fetch()
    {
        Describe("downloading report"); // an optional one-liner for the log
        UF_ASYNC(Download, 0);
        return UF_NEXT(OnPipe_Parse);
    }

private:
    StepResult OnPipe_Parse()
    {
        int rows = AsyncResult<int>().value();
        Describe("parsed ", rows, " rows");
        return UF_NEXT(OnPipe_Store);
    }
    StepResult OnPipe_Store()
    {
        Describe("committed to db");
        return Done();
    }
    static int Download(int)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
        return 1234;
    }
};

// A custom observer: override only the callbacks you care about. Every step
// transition of every module funnels through OnStepChanged; threshold alarms
// arrive via OnSlowCpuStep.
class Metrics : public uniflow::IUniflowObserver
{
public:
    void OnStepChanged(std::string_view /*obj*/, std::string_view /*prev_step*/,
                       std::string_view /*next_step*/, std::string_view /*desc*/,
                       int /*ordinal*/, double /*elapsed_ms*/,
                       const uniflow::TickStats& /*ticks*/) override
    {
        // prometheus_step_ms.observe({obj, prev_step}, elapsed_ms);
        ++step_count_;
    }

    void OnSlowCpuStep(std::string_view /*obj*/, std::string_view /*step*/,
                       double /*cpu_ms*/) override
    {
        // alert(obj, step, cpu_ms);   // "this step held the pump too long"
    }

    int step_count_ = 0;
};

int main()
{
    // Part 1: default observer logs the whole flow for free.
    {
        uniflow::Runtime rt; // default observer = ConsoleObserver
        Pipe             p{rt};
        UF_START_FLOW(p, OnPipe_Fetch);
        p.WaitUntilIdle();
    }

    // Part 2: the same flow, measured by a custom observer.
    {
        uniflow::Runtime::Opts opts;
        opts.observer = std::make_unique<Metrics>();
        uniflow::Runtime rt{std::move(opts)};
        Pipe             p{rt};
        UF_START_FLOW(p, OnPipe_Fetch);
        p.WaitUntilIdle();
    }
    return 0;
}
