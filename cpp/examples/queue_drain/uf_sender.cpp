#include "uf_sender.h"

#include "app.h"
#include "mailbox.h"
#include "uf_receiver.h"

using namespace uniflow;

Flow_Sender::Flow_Sender(uniflow::Runtime& rt)
    : uniflow::Uniflow<Flow_Sender>(rt, "Flow_Sender"),
      rng_(std::random_device{}())
{
    AddTask(ctx_emit_);
    FillVectors();
}

void Flow_Sender::FillVectors()
{
    std::uniform_int_distribution<int> v(1, GlobalConfig::kValueMax);
    vec_a_.reserve(GlobalConfig::kVecSize);
    vec_b_.reserve(GlobalConfig::kVecSize);
    for (int i = 0; i < GlobalConfig::kVecSize; ++i)
    {
        vec_a_.push_back(v(rng_));
        vec_b_.push_back(v(rng_));
    }
}

StepResult Flow_Sender::Task_Emit::Step1_Tick()
{
    if (GlobalEnv::Stop())
    {
        Describe("stop requested -> done");
        return Done();
    }
    if (flow().total_bursts_ >= GlobalConfig::kMaxBurstCount)
    {
        // Burst budget spent: stop emitting so the demo settles. The receiver
        // drains the final burst and parks; the dashboard keeps running.
        Describe("burst budget exhausted -> done");
        return Done();
    }

    // Throttle on wall time. gap_ was armed by OnEnter and survives Stay
    // re-entries, so Passed() measures from task entry / last Restart.
    if (!gap_.Passed(GlobalConfig::kSendGap))
    {
        Describe("idle gap");
        return Stay();   // re-poll this step next round
    }

    flow().EmitBurst();
    gap_.Restart();

    // Wake the receiver if it has parked. Same pump thread, so IsIdle() and
    // StartFlow() are plain in-thread calls - no lock, no cross-thread signal.
    auto& recv = App::inst().recv;
    if (recv.IsIdle())
    {
        recv.ctx_drain_.StartFlow();
    }

    return Stay();
}

void Flow_Sender::EmitBurst()
{
    std::uniform_int_distribution<int> burst(GlobalConfig::kBurstMin,
                                             GlobalConfig::kBurstMax);
    std::uniform_int_distribution<int> idx(0, GlobalConfig::kVecSize - 1);
    std::uniform_int_distribution<int> opbit(0, 1);

    int n             = burst(rng_);
    last_burst_count_ = n;
    ++total_bursts_;

    for (int i = 0; i < n; ++i)
    {
        Msg m;
        m.a  = vec_a_[idx(rng_)];
        m.b  = vec_b_[idx(rng_)];
        m.op = opbit(rng_) == 0 ? '+' : '-';
        Mailbox::Push(m);   // lock-free: only this pump thread touches the queue
    }
    Describe("burst pushed: ", n, " jobs (queue=", Mailbox::Size(), ")");
}
