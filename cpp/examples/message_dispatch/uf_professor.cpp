#include "uf_professor.h"

#include "app.h"
#include "mailbox.h"
#include "uf_student.h"

#include <iostream>

UF_Professor::UF_Professor(uniflow::Runtime& rt)
    : uniflow::Uniflow<UF_Professor>(rt),
      rng_(1)
{
    tasks_ = {
        {Message::Kind::Assignment, "essay",   2, 4, 0},
        {Message::Kind::Assignment, "lab",     5, 6, 0},
        {Message::Kind::Assignment, "project", 8, 9, 0},
        {Message::Kind::Assignment, "review",  3, 2, 0},
    };
}

UF_Professor::StepResult UF_Professor::OnProf_Begin()
{
    ScheduleNext();
    Describe("armed, will emit ", tasks_.size(), " assignments");
    return UF_NEXT(OnProf_Tick);
}

UF_Professor::StepResult UF_Professor::OnProf_Tick()
{
    if (emitted_ >= static_cast<int>(tasks_.size()))
    {
        Describe("all assignments handed out");
        return Done();
    }
    if (uniflow::Clock::now() < next_at_)
    {
        Describe("between assignments");
        return Stay();
    }
    EmitOne();
    ScheduleNext();

    auto& student = App::inst().student;
    if (student.IsIdle())
        UF_START_FLOW(student, OnStudent_Begin);

    return Stay();
}

void UF_Professor::ScheduleNext()
{
    auto min_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      GlobalConfig::kProfMinGap).count();
    auto max_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      GlobalConfig::kProfMaxGap).count();
    std::uniform_int_distribution<long long> d(min_ms, max_ms);
    next_at_ = uniflow::Clock::now() + std::chrono::milliseconds(d(rng_));
}

void UF_Professor::EmitOne()
{
    const Message& m = tasks_[emitted_];
    Mailbox::Push(m);
    ++emitted_;
    std::cout << "[professor] -> assignment \"" << m.name
              << "\" (need_ability=" << m.need_ability
              << ", need_time=" << m.need_time << "h)"
              << "  inbox=" << Mailbox::Size() << "\n";
    Describe("posted \"", m.name, "\"  inbox=", Mailbox::Size());
}
