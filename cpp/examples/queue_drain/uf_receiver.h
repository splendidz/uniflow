// uf_receiver.h - the "processing thread that drains the inbox" half.
// One job per step pass: pop one Msg, dispatch to Add or Sub routine,
// then loop back. When the queue empties the flow Done()s; the sender
// kicks a fresh flow off on the next burst.
#pragma once

#include "globals.h"
#include "uniflow.hpp"

#include <string>

enum class RecvState
{
    Idle,
    Dispatching,
    Adding,
    Subtracting,
};

const char* ToString(RecvState s);

class UF_Receiver : public uniflow::Uniflow<UF_Receiver>
{
    UF_UNIFLOW_IMPLEMENT(UF_Receiver);

public:
    explicit UF_Receiver(uniflow::Runtime& rt)
        : uniflow::Uniflow<UF_Receiver>(rt) {}

    StepResult OnRecv_Begin();

    // Read-only state for visualization (same pump thread).
    RecvState          State()      const { return state_; }
    int                Processed()  const { return processed_; }
    const std::string& LastResult() const { return last_result_; }
    Msg                Current()    const { return current_; }

private:
    StepResult OnRecv_TakeNext();
    StepResult OnRecv_Add();
    StepResult OnRecv_Sub();

    RecvState   state_      = RecvState::Idle;
    Msg         current_{};
    int         processed_  = 0;
    std::string last_result_;
};
