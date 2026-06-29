// uf_sender.h - the "feed thread that stuffs an inbox" half.
//
// One perpetual task (Emit): every kSendGap, push a random burst of 1..N jobs
// into the mailbox, then relaunch the receiver if it has parked. Because both
// modules run on the same pump thread, pushing to the mailbox and calling
// receiver.IsIdle() / StartFlow() inline are all lock-free, in-thread calls.
#pragma once

#include "globals.h"

#include <random>
#include <vector>

class Flow_Sender : public uniflow::Uniflow<Flow_Sender>
{
public:
    explicit Flow_Sender(uniflow::Runtime& rt);

    // The single perpetual emit task (public so app.Start() launches it with
    // task.StartFlow()). Its step re-arms the burst timer on task entry.
    struct Task_Emit : uniflow::Task<Flow_Sender>
    {
        void OnEnter() override { gap_.Restart(); } // re-arm the burst timer
        StepResult Entry() override { return Step1_Tick(); }

    private:
        uniflow::UFTimer gap_;   // wall-clock throttle between bursts

        StepResult Step1_Tick();
    } task_emit_;

    // Read on the snapshot step (same pump thread). Safe, no lock.
    const std::vector<int>& VecA() const { return vec_a_; }
    const std::vector<int>& VecB() const { return vec_b_; }
    int  LastBurstCount() const { return last_burst_count_; }
    int  TotalBursts()    const { return total_bursts_; }

private:
    void FillVectors();
    void EmitBurst();

    // Flow-owned state, reached from the step via flow().member_.
    std::vector<int> vec_a_;
    std::vector<int> vec_b_;
    int              last_burst_count_ = 0;
    int              total_bursts_     = 0;

    std::mt19937 rng_;
};
