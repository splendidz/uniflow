// uf_receiver.h - the "worker that drains the inbox" half.
//
// One looping task (Drain): pop one Msg, dispatch to the Add or Sub step by its
// operator, then loop back to pop the next. When the queue empties the task
// Done()s and the module parks; the sender relaunches it on the next burst.
// State the steps share (the current job, counters) lives on the flow and is
// reached from the steps via flow().
#pragma once

#include "globals.h"

#include <string>

enum class RecvState
{
    Idle,
    Dispatching,
    Adding,
    Subtracting,
};

const char* ToString(RecvState s);

class Flow_Receiver : public uniflow::Uniflow<Flow_Receiver>
{
public:
    explicit Flow_Receiver(uniflow::Runtime& rt);

    // The single drain task. Public so the sender can relaunch it with
    // ctx.StartFlow() when it parks and a new burst arrives.
    struct Task_Drain : uniflow::Task<Flow_Receiver>
    {
        StepResult Entry() override { return Step1_TakeNext(); }

    private:
        StepResult Step1_TakeNext();   // pop one job and dispatch by operator
        StepResult Step2_Add();        // a + b
        StepResult Step3_Sub();        // a - b
    } ctx_drain_;

    // Read-only state for the snapshot step (same pump thread, no lock).
    RecvState          State()      const { return state_; }
    int                Processed()  const { return processed_; }
    const std::string& LastResult() const { return last_result_; }
    Msg                Current()    const { return current_; }

private:
    // Flow-owned state, reached from the steps via flow().member_.
    RecvState   state_     = RecvState::Idle;
    Msg         current_{};
    int         processed_ = 0;
    std::string last_result_;
};
