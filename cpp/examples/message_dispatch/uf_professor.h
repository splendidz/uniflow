// uf_professor.h - "spawner" module that pushes assignment Messages into the
// shared mailbox at random intervals, then ends after its preset list drains.
// After each push it wakes the student if the student parked.
//
// CURRENT API: a module is a uniflow::Uniflow<Flow_X> that owns one or more
// Task structs (each a uniflow::Task<Flow_X>). Declarations only live here;
// every definition - including the step bodies - is in the .cpp.
#pragma once

#include "globals.h"
#include "uniflow.hpp"

#include <random>
#include <vector>

class Flow_Professor : public uniflow::Uniflow<Flow_Professor>
{
public:
    explicit Flow_Professor(uniflow::Runtime& rt);

    int Emitted() const { return emitted_; }
    int Total()   const { return static_cast<int>(tasks_.size()); }
    const std::vector<Message>& Tasks() const { return tasks_; }

    // The single emitting task. Public so App::Start() launches it with
    // task_emit_.StartFlow().
    struct Task_Emit : uniflow::Task<Flow_Professor>
    {
        StepResult Entry() override { return Step1_Arm(); }

    private:
        StepResult Step1_Arm();    // schedule the first emission
        StepResult Step2_Tick();   // poll the gap; emit when due; Done when drained
    } task_emit_;

private:
    void ScheduleNext();
    void EmitOne();

    std::vector<Message> tasks_;
    int                  emitted_ = 0;
    uniflow::TimePoint   next_at_;
    std::mt19937         rng_;
};
