// uf_professor.h - "spawner" module that pushes assignment messages into
// the mailbox at random intervals. Ends after the preset list is empty.
// After each push it wakes the student if it parked.
#pragma once

#include "globals.h"
#include "uniflow.hpp"

#include <random>
#include <vector>

class UF_Professor : public uniflow::Uniflow<UF_Professor>
{
    UF_UNIFLOW_IMPLEMENT(UF_Professor);

public:
    explicit UF_Professor(uniflow::Runtime& rt);

    StepResult OnProf_Begin();

    int Emitted()   const { return emitted_; }
    int Remaining() const { return static_cast<int>(tasks_.size()) - emitted_; }
    int Total()     const { return static_cast<int>(tasks_.size()); }
    const std::vector<Message>& Tasks() const { return tasks_; }

private:
    StepResult OnProf_Tick();

    void ScheduleNext();
    void EmitOne();

    std::vector<Message> tasks_;
    int                  emitted_ = 0;
    uniflow::TimePoint   next_at_;
    std::mt19937         rng_;
};
