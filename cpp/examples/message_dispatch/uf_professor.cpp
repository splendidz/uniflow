#include "uf_professor.h"

#include "app.h"
#include "mailbox.h"
#include "uf_student.h"

using namespace uniflow;

Flow_Professor::Flow_Professor(uniflow::Runtime& rt)
    : uniflow::Uniflow<Flow_Professor>(rt, "Flow_Professor"),
      rng_(1)
{
    // AddTask wires the task's flow() back-pointer so its steps reach this flow
    // and so task_emit_.StartFlow() knows which module to launch.
    AddTask(task_emit_);

    tasks_ = {
        {Message::Kind::Assignment, "essay",   2, 4, 0},
        {Message::Kind::Assignment, "lab",     5, 6, 0},
        {Message::Kind::Assignment, "project", 8, 9, 0},
        {Message::Kind::Assignment, "review",  3, 2, 0},
    };
}

StepResult Flow_Professor::Task_Emit::Step1_Arm()
{
    flow().ScheduleNext();
    Describe("armed, will emit assignments");
    // Next: advance to the next step of THIS task on the next pump round.
    return Next(UF_FN(Step2_Tick));
}

StepResult Flow_Professor::Task_Emit::Step2_Tick()
{
    if (flow().emitted_ >= flow().Total())
    {
        Describe("all assignments handed out");
        return Done();
    }
    if (uniflow::Clock::now() < flow().next_at_)
    {
        Describe("between assignments");
        return Stay();   // re-poll this step next round
    }
    flow().EmitOne();
    flow().ScheduleNext();

    // Wake the student if it parked (cross-module launch via the App singleton).
    Flow_Student& student = App::inst().student;
    if (student.IsIdle())
    {
        student.task_drain_.StartFlow();
    }
    return Stay();
}

void Flow_Professor::ScheduleNext()
{
    auto min_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      GlobalConfig::kProfMinGap).count();
    auto max_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      GlobalConfig::kProfMaxGap).count();
    std::uniform_int_distribution<long long> d(min_ms, max_ms);
    next_at_ = uniflow::Clock::now() + std::chrono::milliseconds(d(rng_));
}

void Flow_Professor::EmitOne()
{
    const Message& m = tasks_[emitted_];
    Mailbox::Push(m);   // lock-free: only the pump thread touches the mailbox
    ++emitted_;
    GlobalLog::Add("professor posted assignment \"" + m.name + "\"  inbox="
                   + std::to_string(Mailbox::Size()));
    Describe("posted \"", m.name, "\"  inbox=", Mailbox::Size());
}
