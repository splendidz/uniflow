#include "uf_friend.h"

#include "app.h"
#include "mailbox.h"
#include "uf_student.h"

using namespace uniflow;

Flow_Friend::Flow_Friend(uniflow::Runtime& rt)
    : uniflow::Uniflow<Flow_Friend>(rt, "Flow_Friend"),
      rng_(2)
{
    AddTask(ctx_emit_);

    plays_ = {
        {Message::Kind::Play, "soccer",     0, 0, 6},
        {Message::Kind::Play, "board game", 0, 0, 3},
        {Message::Kind::Play, "movie",      0, 0, 4},
    };
}

StepResult Flow_Friend::Task_Emit::Step1_Arm()
{
    flow().ScheduleNext();
    Describe("armed, will invite to play");
    return Next(UF_FN(Step2_Tick));
}

StepResult Flow_Friend::Task_Emit::Step2_Tick()
{
    if (flow().emitted_ >= flow().Total())
    {
        Describe("all invites sent");
        return Done();
    }
    if (uniflow::Clock::now() < flow().next_at_)
    {
        Describe("waiting before next invite");
        return Stay();
    }
    flow().EmitOne();
    flow().ScheduleNext();

    Flow_Student& student = App::inst().student;
    if (student.IsIdle())
    {
        student.ctx_drain_.StartFlow();
    }
    return Stay();
}

void Flow_Friend::ScheduleNext()
{
    auto min_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      GlobalConfig::kFriendMinGap).count();
    auto max_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      GlobalConfig::kFriendMaxGap).count();
    std::uniform_int_distribution<long long> d(min_ms, max_ms);
    next_at_ = uniflow::Clock::now() + std::chrono::milliseconds(d(rng_));
}

void Flow_Friend::EmitOne()
{
    const Message& m = plays_[emitted_];
    Mailbox::Push(m);
    ++emitted_;
    GlobalLog::Add("friend posted play \"" + m.name + "\"  inbox="
                   + std::to_string(Mailbox::Size()));
    Describe("posted \"", m.name, "\"  inbox=", Mailbox::Size());
}
