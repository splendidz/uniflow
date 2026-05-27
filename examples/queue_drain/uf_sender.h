// uf_sender.h - mimics the "receive thread that stuffs an inbox" half.
// One flow: every kSendGap, dequeue a random number of (a,b,op) pairs
// into the mailbox, then wake the receiver if it parked.
#pragma once

#include "globals.h"
#include "uniflow.hpp"

#include <random>
#include <vector>

class UF_Sender : public uniflow::Uniflow<UF_Sender>
{
    UF_UNIFLOW_IMPLEMENT(UF_Sender);

public:
    explicit UF_Sender(uniflow::Runtime& rt);

    StepResult OnSend_Begin();

    // Read on the viz step (same pump thread). Safe.
    const std::vector<int>& VecA() const { return vec_a_; }
    const std::vector<int>& VecB() const { return vec_b_; }
    int  LastBurstCount() const { return last_burst_count_; }
    int  TotalBursts()    const { return total_bursts_; }

private:
    StepResult OnSend_Tick();

    void FillVectors();
    void EmitBurst();

    std::vector<int>   vec_a_;
    std::vector<int>   vec_b_;
    int                last_burst_count_ = 0;
    int                total_bursts_     = 0;
    uniflow::TimePoint next_send_at_;

    std::mt19937 rng_;
};
