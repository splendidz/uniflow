#include "uf_sender.h"

#include "app.h"
#include "mailbox.h"
#include "uf_receiver.h"

#include <iostream>

UF_Sender::UF_Sender(uniflow::Runtime& rt)
    : uniflow::Uniflow<UF_Sender>(rt),
      rng_(std::random_device{}())
{
    FillVectors();
}

void UF_Sender::FillVectors()
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

UF_Sender::StepResult UF_Sender::OnSend_Begin()
{
    next_send_at_ = uniflow::Clock::now();
    Describe("sender armed");
    return UF_NEXT(OnSend_Tick);
}

UF_Sender::StepResult UF_Sender::OnSend_Tick()
{
    if (GlobalEnv::Stop())
    {
        Describe("stop requested -> done");
        return Done();
    }
    if (total_bursts_ >= GlobalConfig::kMaxBurstCount)
    {
        // Stop here so the demo eventually ends. The receiver will
        // drain the last burst and park; main waits for both to idle.
        Describe("burst budget exhausted -> done");
        return Done();
    }
    if (uniflow::Clock::now() < next_send_at_)
    {
        Describe("idle gap");
        return Stay();
    }

    EmitBurst();
    next_send_at_ = uniflow::Clock::now() + GlobalConfig::kSendGap;

    // Wake the receiver if it has parked. Same pump thread, so
    // IsIdle() / StartFlow can be called inline.
    auto& recv = App::inst().recv;
    if (recv.IsIdle())
        UF_START_FLOW(recv, OnRecv_Begin);

    return Stay();
}

void UF_Sender::EmitBurst()
{
    std::uniform_int_distribution<int> burst(GlobalConfig::kBurstMin,
                                             GlobalConfig::kBurstMax);
    std::uniform_int_distribution<int> idx(0, GlobalConfig::kVecSize - 1);
    std::uniform_int_distribution<int> opbit(0, 1);

    int n = burst(rng_);
    last_burst_count_ = n;
    ++total_bursts_;

    for (int i = 0; i < n; ++i)
    {
        Msg m;
        m.a  = vec_a_[idx(rng_)];
        m.b  = vec_b_[idx(rng_)];
        m.op = opbit(rng_) == 0 ? '+' : '-';
        Mailbox::Push(m);
    }
    std::cout << "[sender] burst #" << total_bursts_
              << " pushed " << n << " jobs (queue=" << Mailbox::Size() << ")\n";
    Describe("burst pushed: ", n, " jobs (queue=", Mailbox::Size(), ")");
}
