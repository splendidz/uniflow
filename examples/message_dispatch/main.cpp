// ============================================================================
//  message_dispatch — demo_concept.md problem 1, solved with uniflow
//
//  "I am one person, a student, a shared resource. My resources are time,
//   learning ability, and stress. A professor sends assignment messages; a
//   friend sends play messages. A scheduler distributes the messages."
//
//  Why uniflow fits:
//   - The student is a SINGLE shared resource. Modelled as a UF_SINGLETON, its
//     time / ability / stress are touched only on the one pump thread — no
//     locks, no data races, even though messages arrive from client threads.
//   - Each kind of effort (train, sleep, play, do the assignment) is a chain
//     of steps. "Spend N hours" is genuinely blocking, so it is offloaded with
//     UF_ASYNC — the pump thread is never blocked.
//   - The Scheduler is a plain mailbox: clients Post() from their own threads;
//     a Post wakes the parked student. No busy-poll — when the inbox is empty
//     the student goes idle and the pump sleeps.
//
//  Build (from the repo root):
//    cl /std:c++17 /EHsc /utf-8 /I include examples\message_dispatch\main.cpp ^
//       /Fe:build\message_dispatch.exe
//    g++ -std=c++17 -O2 -pthread -I include ^
//       examples/message_dispatch/main.cpp -o build/message_dispatch
// ============================================================================
#include "uniflow.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>

// One simulated hour of effort costs this much real time on the pool.
static constexpr int kHourMs    = 8;
// Stress at or above this forces the student to sleep before training again.
static constexpr int kStressMax = 10;

// ── A client message — what the professor or the friend sends ───────────────
struct Message
{
    enum class Kind { Assignment, Play };
    Kind        kind;
    std::string name;
    int         need_ability = 0; // Assignment: ability the task demands
    int         need_time    = 0; // Assignment: hours to actually do it
    int         play_hours   = 0; // Play: hours of playing
};

class Student; // defined below; Scheduler wakes it

// ════════════════════════════════════════════════════════════════════════════
//  Scheduler — the "scheduling class". A plain thread-safe mailbox: it does not
//  run a flow itself. Clients Post() messages; it hands them out one at a time
//  and wakes the student when a message arrives to a parked student.
// ════════════════════════════════════════════════════════════════════════════
class Scheduler
{
public:
    static Scheduler& inst()
    {
        static Scheduler s;
        return s;
    }

    // Called by client threads. The lock guards only the mailbox hand-off.
    void Post(Message m);

    // Called by the student (on the pump thread) to fetch the next message.
    // Returns false when the inbox is empty — the student then parks.
    bool TakeNext(Message& out);

    // After all clients are done, lets the student's final batch drain & stop.
    void Close() { closed_ = true; }
    bool closed() const { return closed_; }

private:
    Scheduler() = default;

    std::mutex          mu_;
    std::deque<Message> inbox_;
    bool                parked_ = true; // true == student idle, needs a wake
    bool                closed_ = false;
};

// ════════════════════════════════════════════════════════════════════════════
//  Student — the single shared resource. One instance, ever (UF_SINGLETON).
//  One flow drains the whole mailbox: OnNext pulls a message, the matching
//  chain handles it, then control returns to OnNext for the next one.
// ════════════════════════════════════════════════════════════════════════════
class Student : public uniflow::Uniflow<Student>
{
    UF_SINGLETON(Student);

public:
    // Entry point and dispatch hub. Started by Scheduler::Post when parked,
    // and re-entered (UF_NEXT) after every message the student finishes.
    StepResult OnNext()
    {
        Message m;
        if (!Scheduler::inst().TakeNext(m))
        {
            std::cout << "  [student] mailbox empty -> resting\n";
            return Done(); // inbox drained — go idle; the pump sleeps
        }
        current_ = std::move(m);
        if (current_.kind == Message::Kind::Assignment)
        {
            std::cout << "  [student] take assignment \"" << current_.name
                      << "\" (needs ability " << current_.need_ability << ", "
                      << current_.need_time << "h)\n";
            return UF_NEXT(OnAssign_CheckAbility);
        }
        std::cout << "  [student] take play \"" << current_.name << "\" ("
                  << current_.play_hours << "h)\n";
        return UF_NEXT(OnPlay_Do);
    }

    // Read-only state, queried by the Scheduler on the same pump thread.
    int stress() const { return stress_; }
    int ability() const { return ability_; }
    int hours_spent() const { return hours_spent_; }

private:
    // ----- assignment: train / sleep until able, then do the work -----
    StepResult OnAssign_CheckAbility()
    {
        if (ability_ >= current_.need_ability)
            return UF_NEXT(OnAssign_Work); // strong enough — do it
        if (stress_ >= kStressMax)
            return UF_NEXT(OnAssign_Sleep); // too stressed to study
        return UF_NEXT(OnAssign_Train);
    }

    StepResult OnAssign_Train()
    {
        UF_ASYNC(SimHours, 3); // +1 ability costs 3 hours
        return UF_NEXT(OnAssign_TrainDone);
    }
    StepResult OnAssign_TrainDone()
    {
        if (AsyncResult<int>().failed())
            return Fail();
        ability_++;
        stress_++;
        hours_spent_ += 3;
        std::cout << "  [student] trained -> ability " << ability_
                  << ", stress " << stress_ << "\n";
        return UF_NEXT(OnAssign_CheckAbility); // loop
    }

    StepResult OnAssign_Sleep()
    {
        UF_ASYNC(SimHours, 6); // 6 hours of sleep
        return UF_NEXT(OnAssign_SleepDone);
    }
    StepResult OnAssign_SleepDone()
    {
        if (AsyncResult<int>().failed())
            return Fail();
        stress_--;
        hours_spent_ += 6;
        std::cout << "  [student] slept -> stress " << stress_ << "\n";
        return UF_NEXT(OnAssign_CheckAbility); // re-check, sleep more if needed
    }

    StepResult OnAssign_Work()
    {
        UF_ASYNC(SimHours, current_.need_time);
        return UF_NEXT(OnAssign_WorkDone);
    }
    StepResult OnAssign_WorkDone()
    {
        if (AsyncResult<int>().failed())
            return Fail();
        hours_spent_ += current_.need_time;
        std::cout << "  [student] FINISHED assignment \"" << current_.name
                  << "\"\n";
        return UF_NEXT(OnNext); // back to the mailbox
    }

    // ----- play: playing burns off stress -----
    StepResult OnPlay_Do()
    {
        UF_ASYNC(SimHours, current_.play_hours);
        return UF_NEXT(OnPlay_Done);
    }
    StepResult OnPlay_Done()
    {
        if (AsyncResult<int>().failed())
            return Fail();
        int relief = current_.play_hours / 3; // 3h of play -> -1 stress
        stress_    = std::max(0, stress_ - relief);
        hours_spent_ += current_.play_hours;
        std::cout << "  [student] played \"" << current_.name
                  << "\" -> stress " << stress_ << "\n";
        return UF_NEXT(OnNext); // back to the mailbox
    }

    // Simulated blocking work, run on the pool — never on the pump thread.
    // It only sleeps; it touches no member state (UF_ASYNC forbids `this`).
    static int SimHours(int hours)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(hours * kHourMs));
        return hours;
    }

    Message current_;
    int     hours_spent_ = 0;
    int     ability_     = 0;
    int     stress_      = 0;
};

// ── Scheduler methods that need Student to be a complete type ────────────────
void Scheduler::Post(Message m)
{
    bool wake = false;
    {
        std::lock_guard<std::mutex> lk(mu_);
        inbox_.push_back(std::move(m));
        if (parked_) // student was idle — this message must wake it
        {
            parked_ = false;
            wake    = true;
        }
    }
    // Start() is thread-safe; it serialises with the pump. By the time a
    // parked student is woken, its previous flow has fully ended.
    if (wake)
        Student::inst().Start(&Student::OnNext);
}

bool Scheduler::TakeNext(Message& out)
{
    std::lock_guard<std::mutex> lk(mu_);
    if (inbox_.empty())
    {
        parked_ = true; // claimed under the lock, paired with Post's wake
        return false;
    }
    // "Distribute appropriately" — no optimiser, just a sensible hand-off:
    // when the student is near burnout, let a queued Play jump the line.
    if (Student::inst().stress() >= kStressMax - 3)
    {
        auto it = std::find_if(inbox_.begin(), inbox_.end(), [](const Message& m)
                               { return m.kind == Message::Kind::Play; });
        if (it != inbox_.end())
        {
            std::cout << "  [scheduler] student stressed -> play goes first\n";
            out = std::move(*it);
            inbox_.erase(it);
            return true;
        }
    }
    out = std::move(inbox_.front());
    inbox_.pop_front();
    return true;
}

// ════════════════════════════════════════════════════════════════════════════
//  main — the professor and the friend are two independent client threads.
// ════════════════════════════════════════════════════════════════════════════
int main()
{
    std::cout << "=== message_dispatch: a student, a scheduler, two clients ==="
              << "\n\n";

    // Client thread 1 — the professor hands out assignments.
    std::thread professor([]
    {
        const Message tasks[] = {
            {Message::Kind::Assignment, "essay",   2, 4},
            {Message::Kind::Assignment, "lab",     5, 6},
            {Message::Kind::Assignment, "project", 8, 9},
        };
        std::mt19937                       rng(1);
        std::uniform_int_distribution<int> gap(20, 70);
        for (const auto& t : tasks)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(gap(rng)));
            std::cout << "[professor] -> assignment \"" << t.name << "\"\n";
            Scheduler::inst().Post(t);
        }
    });

    // Client thread 2 — the friend invites the student to play.
    std::thread buddy([]
    {
        const Message plays[] = {
            {Message::Kind::Play, "soccer",     0, 0, 6},
            {Message::Kind::Play, "board game", 0, 0, 3},
        };
        std::mt19937                       rng(2);
        std::uniform_int_distribution<int> gap(30, 90);
        for (const auto& p : plays)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(gap(rng)));
            std::cout << "[friend] -> play \"" << p.name << "\"\n";
            Scheduler::inst().Post(p);
        }
    });

    professor.join();
    buddy.join();
    Scheduler::inst().Close(); // no more messages will arrive

    // Wait for the student to drain whatever is still queued. The student may
    // have parked between batches; loop until the mailbox is truly empty.
    for (;;)
    {
        Student::inst().Wait();
        if (Scheduler::inst().closed() && Student::inst().IsIdle())
            break;
    }

    const Student& s = Student::inst();
    std::cout << "\n=== done: " << s.hours_spent() << "h spent, ability "
              << s.ability() << ", stress " << s.stress() << " ===\n";
    return 0;
}
