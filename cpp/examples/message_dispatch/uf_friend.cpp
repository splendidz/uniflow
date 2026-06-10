#include "uf_friend.h"

#include "app.h"
#include "mailbox.h"
#include "uf_student.h"

#include <iostream>

UF_Friend::UF_Friend(uniflow::Runtime& rt)
    : uniflow::Uniflow<UF_Friend>(rt),
      rng_(2)
{
    plays_ = {
        {Message::Kind::Play, "soccer",     0, 0, 6},
        {Message::Kind::Play, "board game", 0, 0, 3},
        {Message::Kind::Play, "movie",      0, 0, 4},
    };
}

UF_Friend::StepResult UF_Friend::OnFriend_Begin()
{
    ScheduleNext();
    Describe("armed, will invite ", plays_.size(), " times");
    return UF_NEXT(OnFriend_Tick);
}

UF_Friend::StepResult UF_Friend::OnFriend_Tick()
{
    if (emitted_ >= static_cast<int>(plays_.size()))
    {
        Describe("all invites sent");
        return Done();
    }
    if (uniflow::Clock::now() < next_at_)
    {
        Describe("waiting before next invite");
        return Stay();
    }
    EmitOne();
    ScheduleNext();

    auto& student = App::inst().student;
    if (student.IsIdle())
        UF_START_FLOW(student, OnStudent_Begin);

    return Stay();
}

void UF_Friend::ScheduleNext()
{
    auto min_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      GlobalConfig::kFriendMinGap).count();
    auto max_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      GlobalConfig::kFriendMaxGap).count();
    std::uniform_int_distribution<long long> d(min_ms, max_ms);
    next_at_ = uniflow::Clock::now() + std::chrono::milliseconds(d(rng_));
}

void UF_Friend::EmitOne()
{
    const Message& m = plays_[emitted_];
    Mailbox::Push(m);
    ++emitted_;
    std::cout << "[friend] -> play \"" << m.name
              << "\" (" << m.play_hours << "h)"
              << "  inbox=" << Mailbox::Size() << "\n";
    Describe("posted \"", m.name, "\"  inbox=", Mailbox::Size());
}
