// uf_friend.h - second spawner: pushes play messages into the mailbox.
// Same shape as UF_Professor.
#pragma once

#include "globals.h"
#include "uniflow.hpp"

#include <random>
#include <vector>

class UF_Friend : public uniflow::Uniflow<UF_Friend>
{
    UF_UNIFLOW_IMPLEMENT(UF_Friend);

public:
    explicit UF_Friend(uniflow::Runtime& rt);

    StepResult OnFriend_Begin();

    int Emitted()   const { return emitted_; }
    int Remaining() const { return static_cast<int>(plays_.size()) - emitted_; }
    int Total()     const { return static_cast<int>(plays_.size()); }
    const std::vector<Message>& Plays() const { return plays_; }

private:
    StepResult OnFriend_Tick();

    void ScheduleNext();
    void EmitOne();

    std::vector<Message> plays_;
    int                  emitted_ = 0;
    uniflow::TimePoint   next_at_;
    std::mt19937         rng_;
};
