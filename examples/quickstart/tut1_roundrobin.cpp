// tut1_roundrobin.cpp - the round-robin quickstart from the README.
// Two modules on one Runtime; the pump visits each once per round, so the
// two step chains run one step at a time, interleaved - on a single thread.
//
// Build & run (portable, console only):
//   g++ -std=c++17 -O2 -pthread -I . examples/quickstart/tut1_roundrobin.cpp -o tut1 && ./tut1
//   cl /std:c++17 /EHsc /I . examples\quickstart\tut1_roundrobin.cpp /Fe:tut1.exe && tut1.exe
#include "uniflow.hpp"

#include <iostream>
#include <memory>

// A no-op observer to silence framework logs and show only our output.
struct Silent : uniflow::IUniflowObserver
{
};

// Module A: a 3-step chain.
class Alice : public uniflow::Uniflow<Alice>
{
    UF_UNIFLOW_IMPLEMENT(Alice);

public:
    explicit Alice(uniflow::Runtime& rt) : uniflow::Uniflow<Alice>(rt) {}

    StepResult OnA_Step1()
    {
        std::cout << "[Alice] step 1\n";
        return UF_NEXT(OnA_Step2);
    }

private:
    StepResult OnA_Step2()
    {
        std::cout << "[Alice] step 2\n";
        return UF_NEXT(OnA_Step3);
    }
    StepResult OnA_Step3()
    {
        std::cout << "[Alice] step 3\n";
        return Done();
    }
};

// Module B: another 3-step chain.
class Bob : public uniflow::Uniflow<Bob>
{
    UF_UNIFLOW_IMPLEMENT(Bob);

public:
    explicit Bob(uniflow::Runtime& rt) : uniflow::Uniflow<Bob>(rt) {}

    StepResult OnB_Step1()
    {
        std::cout << "        [Bob] step 1\n";
        return UF_NEXT(OnB_Step2);
    }

private:
    StepResult OnB_Step2()
    {
        std::cout << "        [Bob] step 2\n";
        return UF_NEXT(OnB_Step3);
    }
    StepResult OnB_Step3()
    {
        std::cout << "        [Bob] step 3\n";
        return Done();
    }
};

int main()
{
    uniflow::Runtime::Opts opts;
    opts.observer = std::make_unique<Silent>();
    uniflow::Runtime rt{std::move(opts)};

    Alice a{rt}; // attach both to the same runtime
    Bob   b{rt}; // -> they run cooperatively on the same pump thread

    UF_START_FLOW(a, OnA_Step1);
    UF_START_FLOW(b, OnB_Step1);

    a.WaitUntilIdle();
    b.WaitUntilIdle();
    return 0;
}
