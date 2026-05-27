// uf_student.h - the worker. One flow drains the mailbox: take a Message,
// dispatch by kind, run the matching chain (train / sleep / work for
// assignments; play for invites), then take the next. The flow ends only
// when the mailbox is empty AND both spawners are idle - that way the
// student survives quiet gaps between bursts.
#pragma once

#include "globals.h"
#include "uniflow.hpp"

class UF_Student : public uniflow::Uniflow<UF_Student>
{
    UF_UNIFLOW_IMPLEMENT(UF_Student);

public:
    explicit UF_Student(uniflow::Runtime& rt)
        : uniflow::Uniflow<UF_Student>(rt) {}

    StepResult OnStudent_Begin();

    int Ability()    const { return ability_; }
    int Stress()     const { return stress_; }
    int HoursSpent() const { return hours_spent_; }
    int Done_count() const { return done_count_; }
    const Message& CurrentMessage() const { return current_; }

private:
    // Entry hub: pop one message or park when there is nothing left.
    StepResult OnStudent_TakeNext();

    // ----- assignment chain: train / sleep / do -----
    StepResult OnAssign_CheckAbility();
    StepResult OnAssign_Train();
    StepResult OnAssign_TrainDone();
    StepResult OnAssign_Sleep();
    StepResult OnAssign_SleepDone();
    StepResult OnAssign_Work();
    StepResult OnAssign_WorkDone();

    // ----- play chain: play burns off stress -----
    StepResult OnPlay_Do();
    StepResult OnPlay_Done();

    // Static helper for UF_ASYNC. Simulates 'hours' of blocking work on
    // the pool thread (never on the pump). Has no access to 'this'.
    static int SimHours(int hours);

    bool BothSpawnersDone() const;

    Message current_{};
    int     ability_     = 0;
    int     stress_      = 0;
    int     hours_spent_ = 0;
    int     done_count_  = 0;
};
