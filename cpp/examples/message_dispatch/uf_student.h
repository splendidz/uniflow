// uf_student.h - the worker. One task drains the mailbox: take a Message,
// dispatch by kind, run the matching chain (train / sleep / work for
// assignments; play for invites), then take the next. The task ends only when
// the mailbox is empty AND both spawners are idle - that way the student
// survives quiet gaps between bursts.
//
// FEATURE FOCUS: message routing by Message::Kind, and blocking "SimHours" work
// offloaded to the pool with SubmitAsync, then polled in a later step with
// AsyncResult<int>(id). The AsyncId is carried to the wait step via Next(...).
#pragma once

#include "globals.h"
#include "uniflow.hpp"

class Flow_Student : public uniflow::Uniflow<Flow_Student>
{
public:
    explicit Flow_Student(uniflow::Runtime& rt);

    int Ability()    const { return ability_; }
    int Stress()     const { return stress_; }
    int HoursSpent() const { return hours_spent_; }
    int DoneCount()  const { return done_count_; }
    const Message& CurrentMessage() const { return current_; }

    // The single draining task. Public so spawners / App launch it with
    // task_drain_.StartFlow().
    struct Task_Drain : uniflow::Task<Flow_Student>
    {
        StepResult Entry() override { return Step1_TakeNext(); }

    private:
        // Entry hub: pop one message or park when there is nothing left.
        StepResult Step1_TakeNext();

        // ----- assignment chain: train / sleep / do -----
        StepResult Step2_CheckAbility();
        StepResult Step3_Train();
        StepResult Step4_TrainWait(uniflow::AsyncId job);
        StepResult Step5_Sleep();
        StepResult Step6_SleepWait(uniflow::AsyncId job);
        StepResult Step7_Work();
        StepResult Step8_WorkWait(uniflow::AsyncId job);

        // ----- play chain: play burns off stress -----
        StepResult Step9_Play();
        StepResult Step10_PlayWait(uniflow::AsyncId job);

        // Static helper for SubmitAsync. Simulates 'hours' of blocking work on a
        // pool thread (never on the pump). Has no access to 'this'.
        static int SimHours(int hours);
    } task_drain_;

private:
    bool BothSpawnersDone() const;

    Message current_{};
    int     ability_     = 0;
    int     stress_      = 0;
    int     hours_spent_ = 0;
    int     done_count_  = 0;
};
