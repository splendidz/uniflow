// ============================================================================
//  05_dining_philosophers.cpp — the classic deadlock problem, deadlock-proof
//
//  Five philosophers share five forks; each needs both neighbouring forks to
//  eat. The multi-threaded version is the textbook DEADLOCK example: if every
//  philosopher grabs their left fork and then waits for the right, all five
//  wait forever. Avoiding it needs a lock ordering, an arbitrator, or
//  try-lock-and-back-off.
//
//  Here each fork is just a bool. The "are both forks free? then take them"
//  check is a SINGLE step — and the runtime never interleaves steps — so a
//  philosopher takes BOTH forks or NEITHER, atomically. The deadlock cannot
//  happen, with no lock and no special protocol at all.
//
//  Build (from repo root):
//    cl /std:c++17 /EHsc /I include ^
//       examples\tutorials\05_dining_philosophers.cpp /Fe:build\05_dining_philosophers.exe
//    g++ -std=c++17 -pthread -I include ^
//       examples/tutorials/05_dining_philosophers.cpp -o build/05_dining_philosophers
// ============================================================================
#include "uniflow.hpp"

#include <array>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

// Five forks shared by five philosophers. Multi-threaded, each fork would be
// a mutex; here it is one bool — see Philosopher::OnAcquire for why that is
// enough.
struct Table
{
    std::array<bool, 5> fork_in_use{}; // all false initially
};

class Philosopher : public uniflow::Uniflow<Philosopher>
{
    using S = Philosopher;
    UF_USES_UNIFLOW(Philosopher);

public:
    Philosopher(uniflow::UniflowRuntime& rt, Table& table, int id, int meals)
        : Uniflow(rt), table_(table), id_(id), meals_(meals)
    {
        left_  = id;           // the fork on the philosopher's left
        right_ = (id + 1) % 5; // the fork on the philosopher's right
        UF_ENTRY("Dine", OnThink);
    }

private:
    StepResult OnThink()
    {
        std::cout << "  philosopher " << id_ << " thinks\n";
        return UF_NEXT(OnAcquire);
    }

    StepResult OnAcquire()
    {
        // The whole point: this check-and-take is ONE step. Because no other
        // step can run in the middle of it, a philosopher takes BOTH forks
        // or NEITHER — atomically. The classic deadlock (everyone holds one
        // fork, each waiting for the other) simply cannot arise.
        if (table_.fork_in_use[left_] || table_.fork_in_use[right_])
            return Stay(); // a neighbour holds a fork — yield and retry

        table_.fork_in_use[left_]  = true;
        table_.fork_in_use[right_] = true;
        return UF_NEXT(OnEat);
    }

    StepResult OnEat()
    {
        ++eaten_;
        std::cout << "  philosopher " << id_ << " EATS (meal " << eaten_
                  << " of " << meals_ << ")\n";
        return UF_NEXT(OnRelease);
    }

    StepResult OnRelease()
    {
        table_.fork_in_use[left_]  = false;
        table_.fork_in_use[right_] = false;
        if (eaten_ >= meals_)
        {
            std::cout << "  philosopher " << id_ << " is full - leaves\n";
            return Done();
        }
        return UF_NEXT(OnThink); // loop: think, then eat again
    }

    Table& table_;
    int    id_;
    int    meals_;
    int    left_  = 0;
    int    right_ = 0;
    int    eaten_ = 0;
};

int main()
{
    constexpr int kMeals = 2;

    Table table; // five shared forks — no mutexes

    uniflow::UniflowRuntime rt;
    Philosopher*            phil[5];
    for (int i = 0; i < 5; ++i)
        phil[i] = rt.Create<Philosopher>("phil" + std::to_string(i), table, i, kMeals);

    rt.RunInBackground();

    for (auto* p : phil)
        p->Start("Dine");

    auto all_done = [&]
    {
        for (auto* p : phil)
            if (!p->IsIdle())
                return false;
        return true;
    };
    while (!all_done())
        std::this_thread::sleep_for(std::chrono::milliseconds(2));

    std::cout << "\nevery philosopher ate " << kMeals
              << " meal(s) -- no deadlock, no locks\n";
    return 0;
}
