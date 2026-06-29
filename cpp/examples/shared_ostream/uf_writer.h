// uf_writer.h - one writer module appends its 'text' to the shared
// ostringstream 'count' times. Two instances of Flow_Writer share the same
// Runtime; a shared turn flag in SharedState forces them to alternate so the
// output is well-ordered.
//
// The interesting bit: no mutex anywhere. Both modules run their steps on the
// Runtime's single pump thread, so writes to SharedState::Log() and reads of
// SharedState::Turn() cannot race.
//
// CURRENT API: a flow is a class deriving from uniflow::Uniflow<Flow_Writer>;
// its one Task (Task_Write) owns the steps. AddTask wires the task in the ctor;
// App launches it with module.task_write_.StartFlow().
#pragma once

#include "uniflow.hpp"

#include <string>

class Flow_Writer : public uniflow::Uniflow<Flow_Writer>
{
public:
    // text    - what to append every time it is this writer's turn
    // count   - how many times to append
    // turn_id - 0 or 1; the writer waits while the shared turn flag != turn_id
    Flow_Writer(uniflow::Runtime& rt, std::string text, int count, int turn_id);

    int Remaining() const { return remaining_; }

    // The flow's single task. Public so App launches it with task_write_.StartFlow().
    struct Task_Write : uniflow::Task<Flow_Writer>
    {
        StepResult Entry() override { return Step1_Begin(); }

    private:
        StepResult Step1_Begin();   // announce the work
        StepResult Step2_Loop();    // append on our turn, else Stay until it
    } task_write_;

private:
    // Flow state, reached from steps via flow().member_.
    std::string text_;
    int         remaining_ = 0;
    int         turn_id_   = 0;
};
