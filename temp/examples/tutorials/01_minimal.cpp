// ============================================================================
//  01_minimal.cpp — the smallest useful uniflow module
//
//  Teaches: deriving Uniflow<T>, registering a named flow, the step chain
//           (UF_NEXT), and the two terminal results Done() / Fail().
//           No async here — just a synchronous, ordered sequence of steps.
//
//  Build (from repo root):
//    cl /std:c++17 /EHsc /I include ^
//       examples\tutorials\01_minimal.cpp /Fe:build\01_minimal.exe
//    g++ -std=c++17 -pthread -I include ^
//       examples/tutorials/01_minimal.cpp -o build/01_minimal
// ============================================================================
#include "uniflow.hpp"

#include <chrono>
#include <iostream>
#include <thread>

// A flow is just a chain of step functions on a module. Each step decides,
// inside its own body, which step runs next — so adding, removing, or
// reordering a step is a one-line local change that touches nothing else.
class Preflight : public uniflow::Uniflow<Preflight>
{
    using S = Preflight;        // every module needs this alias...
    UF_USES_UNIFLOW(Preflight); // ...and this, once, in the class body.

public:
    explicit Preflight(uniflow::UniflowRuntime& rt)
        : Uniflow(rt)
    {
        // Register the entry step of a named flow. External callers launch
        // it by name with Start("Startup").
        UF_ENTRY("Startup", OnCheckConfig);
    }

    // Pretend these are real startup checks. Flip one to false and re-run to
    // watch the flow abort at exactly that step.
    bool config_loaded_ = true;
    bool database_up_   = true;
    bool port_free_     = true;

private:
    // Each step returns a StepResult. The framework owns all flow state;
    // a step only expresses intent: advance, finish, or abort.
    StepResult OnCheckConfig()
    {
        std::cout << "  checking configuration...\n";
        if (!config_loaded_)
            return Fail();               // abort the whole flow
        return UF_NEXT(OnCheckDatabase); // ...otherwise advance to next
    }

    StepResult OnCheckDatabase()
    {
        std::cout << "  checking database connection...\n";
        if (!database_up_)
            return Fail();
        return UF_NEXT(OnCheckPort);
    }

    StepResult OnCheckPort()
    {
        std::cout << "  checking listen port...\n";
        if (!port_free_)
            return Fail();
        return UF_NEXT(OnReady);
    }

    StepResult OnReady()
    {
        std::cout << "  all checks passed - service ready\n";
        return Done(); // flow completed normally
    }
};

int main()
{
    uniflow::UniflowRuntime rt;
    auto*                   sys = rt.Create<Preflight>("preflight");

    // The runtime owns its pump thread: RunInBackground() starts it, and the
    // runtime's destructor stops and joins it when main() returns.
    rt.RunInBackground();

    sys->Start("Startup");
    while (!sys->IsIdle())
        std::this_thread::sleep_for(std::chrono::milliseconds(2));

    return 0;
}
