#include "uf_student.h"

#include "app.h"
#include "mailbox.h"
#include "uf_friend.h"
#include "uf_professor.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

using namespace uniflow;

Flow_Student::Flow_Student(uniflow::Runtime& rt)
    : uniflow::Uniflow<Flow_Student>(rt, "Flow_Student")
{
    AddTask(ctx_drain_);
}

bool Flow_Student::BothSpawnersDone() const
{
    return App::inst().prof.IsIdle() && App::inst().friend_.IsIdle();
}

// --- entry hub ---

StepResult Flow_Student::Task_Drain::Step1_TakeNext()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    if (!Mailbox::TryPop(flow().current_))
    {
        if (flow().BothSpawnersDone())
        {
            Describe("mailbox empty and spawners done -> resting for good");
            return Done();
        }
        // Quiet gap; the spawners may still post later. Park-and-wait by
        // Stay()ing rather than Done()ing - cheaper than Done+restart on every
        // message because the pump just polls us at stay_sleep_ms.
        Describe("mailbox empty, spawners not done -> waiting");
        return Stay();
    }

    if (flow().current_.kind == Message::Kind::Assignment)
    {
        // Route by Message::Kind: assignments enter the train/sleep/work chain.
        GlobalLog::Add("student took assignment \"" + flow().current_.name + "\"");
        Describe("took assignment \"", flow().current_.name, "\"");
        return Next(UF_FN(Step2_CheckAbility));
    }
    // Plays enter the play chain.
    GlobalLog::Add("student took play \"" + flow().current_.name + "\"");
    Describe("took play \"", flow().current_.name, "\"");
    return Next(UF_FN(Step9_Play));
}

// --- assignment chain ---

StepResult Flow_Student::Task_Drain::Step2_CheckAbility()
{
    if (flow().ability_ >= flow().current_.need_ability)
    {
        Describe("ability sufficient -> work");
        return Next(UF_FN(Step7_Work));
    }
    if (flow().stress_ >= GlobalConfig::kStressMax)
    {
        Describe("too stressed -> sleep");
        return Next(UF_FN(Step5_Sleep));
    }
    Describe("need more ability -> train");
    return Next(UF_FN(Step3_Train));
}

StepResult Flow_Student::Task_Drain::Step3_Train()
{
    Describe("training (+1 ability, 3h)");
    // Offload the blocking 3h to the pool; carry the AsyncId to the wait step.
    AsyncId job = SubmitAsync(UF_FN(SimHours), Duration::max(), 3);
    if (job == 0)
    {
        return Fail();   // in-flight cap reached
    }
    return Next(UF_FN(Step4_TrainWait), job);
}

StepResult Flow_Student::Task_Drain::Step4_TrainWait(uniflow::AsyncId job)
{
    // Poll the submission by id. While in flight, keep polling this step.
    auto r = AsyncResult<int>(job);
    if (r.pending())
    {
        Describe("training...");
        return Stay();
    }
    if (!r.ok())
    {
        return Fail();
    }
    ++flow().ability_;
    ++flow().stress_;
    flow().hours_spent_ += *r.return_value;
    GlobalLog::Add("student trained -> ability=" + std::to_string(flow().ability_)
                   + " stress=" + std::to_string(flow().stress_));
    Describe("trained: ability=", flow().ability_, " stress=", flow().stress_);
    return Next(UF_FN(Step2_CheckAbility));
}

StepResult Flow_Student::Task_Drain::Step5_Sleep()
{
    Describe("sleeping 6h");
    AsyncId job = SubmitAsync(UF_FN(SimHours), Duration::max(), 6);
    if (job == 0)
    {
        return Fail();
    }
    return Next(UF_FN(Step6_SleepWait), job);
}

StepResult Flow_Student::Task_Drain::Step6_SleepWait(uniflow::AsyncId job)
{
    auto r = AsyncResult<int>(job);
    if (r.pending())
    {
        Describe("sleeping...");
        return Stay();
    }
    if (!r.ok())
    {
        return Fail();
    }
    flow().stress_ = std::max(0, flow().stress_ - 6);
    flow().hours_spent_ += *r.return_value;
    GlobalLog::Add("student slept -> stress=" + std::to_string(flow().stress_));
    Describe("slept: stress=", flow().stress_);
    return Next(UF_FN(Step2_CheckAbility));
}

StepResult Flow_Student::Task_Drain::Step7_Work()
{
    Describe("doing \"", flow().current_.name, "\" for ", flow().current_.need_time, "h");
    AsyncId job = SubmitAsync(UF_FN(SimHours), Duration::max(), flow().current_.need_time);
    if (job == 0)
    {
        return Fail();
    }
    return Next(UF_FN(Step8_WorkWait), job);
}

StepResult Flow_Student::Task_Drain::Step8_WorkWait(uniflow::AsyncId job)
{
    auto r = AsyncResult<int>(job);
    if (r.pending())
    {
        Describe("working...");
        return Stay();
    }
    if (!r.ok())
    {
        return Fail();
    }
    flow().hours_spent_ += *r.return_value;
    ++flow().stress_;
    ++flow().done_count_;
    GlobalLog::Add("student FINISHED assignment \"" + flow().current_.name
                   + "\"  (stress=" + std::to_string(flow().stress_)
                   + ", done=" + std::to_string(flow().done_count_) + ")");
    Describe("finished \"", flow().current_.name, "\"");
    return Next(UF_FN(Step1_TakeNext));
}

// --- play chain ---

StepResult Flow_Student::Task_Drain::Step9_Play()
{
    Describe("playing \"", flow().current_.name, "\" ", flow().current_.play_hours, "h");
    AsyncId job = SubmitAsync(UF_FN(SimHours), Duration::max(), flow().current_.play_hours);
    if (job == 0)
    {
        return Fail();
    }
    return Next(UF_FN(Step10_PlayWait), job);
}

StepResult Flow_Student::Task_Drain::Step10_PlayWait(uniflow::AsyncId job)
{
    auto r = AsyncResult<int>(job);
    if (r.pending())
    {
        Describe("playing...");
        return Stay();
    }
    if (!r.ok())
    {
        return Fail();
    }
    int relief = *r.return_value / 3;
    flow().stress_ = std::max(0, flow().stress_ - relief);
    flow().hours_spent_ += *r.return_value;
    ++flow().done_count_;
    GlobalLog::Add("student played \"" + flow().current_.name + "\" -> stress="
                   + std::to_string(flow().stress_));
    Describe("played \"", flow().current_.name, "\"  stress=", flow().stress_);
    return Next(UF_FN(Step1_TakeNext));
}

int Flow_Student::Task_Drain::SimHours(int hours)
{
    std::this_thread::sleep_for(
        std::chrono::milliseconds(hours * GlobalConfig::kHourMs));
    return hours;
}
