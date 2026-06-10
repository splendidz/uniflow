// uf_writer.h - one writer module appends its 'text' to the shared
// ostringstream 'count' times. Two instances of UF_Writer share the
// same Runtime; the turn flag in SharedState forces them to alternate
// so the output is well-ordered.
//
// The interesting bit: no mutex anywhere. Both modules run their steps
// on the Runtime's single pump thread, so writes to SharedState::Log()
// and reads of SharedState::Turn() cannot race.
#pragma once

#include "uniflow.hpp"

#include <string>

class UF_Writer : public uniflow::Uniflow<UF_Writer>
{
    UF_UNIFLOW_IMPLEMENT(UF_Writer);

public:
    // Entry. Captures the input the user gave at StartFlow time.
    //   text     - what to append every time it is this writer's turn
    //   count    - how many times to append
    //   turn_id  - 0 or 1; the writer waits while turn != turn_id
    StepResult OnWrite_Begin(std::string text, int count, int turn_id);

    int Remaining() const { return remaining_; }

private:
    StepResult OnWrite_Loop();

    std::string text_;
    int         remaining_ = 0;
    int         turn_id_   = 0;
};
