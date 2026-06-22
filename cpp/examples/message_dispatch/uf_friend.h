// uf_friend.h - second spawner: pushes play Messages into the mailbox.
// Same shape as Flow_Professor.
#pragma once

#include "globals.h"
#include "uniflow.hpp"

#include <random>
#include <vector>

class Flow_Friend : public uniflow::Uniflow<Flow_Friend>
{
public:
    explicit Flow_Friend(uniflow::Runtime& rt);

    int Emitted() const { return emitted_; }
    int Total()   const { return static_cast<int>(plays_.size()); }
    const std::vector<Message>& Plays() const { return plays_; }

    struct Task_Emit : uniflow::Task<Flow_Friend>
    {
        StepResult Entry() override { return Step1_Arm(); }

    private:
        StepResult Step1_Arm();
        StepResult Step2_Tick();
    } ctx_emit_;

private:
    void ScheduleNext();
    void EmitOne();

    std::vector<Message> plays_;
    int                  emitted_ = 0;
    uniflow::TimePoint   next_at_;
    std::mt19937         rng_;
};
