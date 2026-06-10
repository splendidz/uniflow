#include "uf_student.h"

#include "app.h"
#include "mailbox.h"
#include "uf_friend.h"
#include "uf_professor.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>

bool UF_Student::BothSpawnersDone() const
{
    return App::inst().prof.IsIdle() && App::inst().friend_.IsIdle();
}

UF_Student::StepResult UF_Student::OnStudent_Begin()
{
    Describe("woken up");
    return UF_NEXT(OnStudent_TakeNext);
}

UF_Student::StepResult UF_Student::OnStudent_TakeNext()
{
    if (!Mailbox::TryPop(current_))
    {
        if (BothSpawnersDone())
        {
            Describe("mailbox empty and spawners done -> resting for good");
            return Done();
        }
        // Quiet gap; the spawners may still post later. Park-and-wait by
        // Stay()ing rather than Done()ing - cheaper than Done+restart on
        // every message because the pump just polls us at stay_sleep_ms.
        Describe("mailbox empty, spawners not done -> waiting");
        return Stay();
    }

    if (current_.kind == Message::Kind::Assignment)
    {
        std::cout << "  [student] take assignment \"" << current_.name
                  << "\" (need_ability=" << current_.need_ability
                  << ", need_time=" << current_.need_time << "h)\n";
        Describe("took assignment \"", current_.name, "\"");
        return UF_NEXT(OnAssign_CheckAbility);
    }
    std::cout << "  [student] take play \"" << current_.name
              << "\" (" << current_.play_hours << "h)\n";
    Describe("took play \"", current_.name, "\"");
    return UF_NEXT(OnPlay_Do);
}

// --- assignment chain ---

UF_Student::StepResult UF_Student::OnAssign_CheckAbility()
{
    if (ability_ >= current_.need_ability)
    {
        Describe("ability sufficient -> work");
        return UF_NEXT(OnAssign_Work);
    }
    if (stress_ >= GlobalConfig::kStressMax)
    {
        Describe("too stressed -> sleep");
        return UF_NEXT(OnAssign_Sleep);
    }
    Describe("need more ability -> train");
    return UF_NEXT(OnAssign_Train);
}

UF_Student::StepResult UF_Student::OnAssign_Train()
{
    Describe("training (+1 ability, 3h)");
    UF_ASYNC(SimHours, 3);
    return UF_NEXT(OnAssign_TrainDone);
}

UF_Student::StepResult UF_Student::OnAssign_TrainDone()
{
    if (AsyncResult<int>().failed())
        return Fail();
    ++ability_;
    ++stress_;
    hours_spent_ += 3;
    std::cout << "  [student] trained -> ability=" << ability_
              << " stress=" << stress_ << "\n";
    Describe("trained: ability=", ability_, " stress=", stress_);
    return UF_NEXT(OnAssign_CheckAbility);
}

UF_Student::StepResult UF_Student::OnAssign_Sleep()
{
    Describe("sleeping 6h");
    UF_ASYNC(SimHours, 6);
    return UF_NEXT(OnAssign_SleepDone);
}

UF_Student::StepResult UF_Student::OnAssign_SleepDone()
{
    if (AsyncResult<int>().failed())
        return Fail();
    stress_ = std::max(0, stress_ - 6);
    hours_spent_ += 6;
    std::cout << "  [student] slept -> stress=" << stress_ << "\n";
    Describe("slept: stress=", stress_);
    return UF_NEXT(OnAssign_CheckAbility);
}

UF_Student::StepResult UF_Student::OnAssign_Work()
{
    Describe("doing \"", current_.name, "\" for ", current_.need_time, "h");
    UF_ASYNC(SimHours, current_.need_time);
    return UF_NEXT(OnAssign_WorkDone);
}

UF_Student::StepResult UF_Student::OnAssign_WorkDone()
{
    if (AsyncResult<int>().failed())
        return Fail();
    hours_spent_ += current_.need_time;
    ++stress_;
    ++done_count_;
    std::cout << "  [student] FINISHED assignment \"" << current_.name
              << "\"  (stress=" << stress_
              << ", done=" << done_count_ << ")\n";
    Describe("finished \"", current_.name, "\"");
    return UF_NEXT(OnStudent_TakeNext);
}

// --- play chain ---

UF_Student::StepResult UF_Student::OnPlay_Do()
{
    Describe("playing \"", current_.name, "\" ", current_.play_hours, "h");
    UF_ASYNC(SimHours, current_.play_hours);
    return UF_NEXT(OnPlay_Done);
}

UF_Student::StepResult UF_Student::OnPlay_Done()
{
    if (AsyncResult<int>().failed())
        return Fail();
    int relief = current_.play_hours / 3;
    stress_    = std::max(0, stress_ - relief);
    hours_spent_ += current_.play_hours;
    ++done_count_;
    std::cout << "  [student] played \"" << current_.name
              << "\" -> stress=" << stress_ << "\n";
    Describe("played \"", current_.name, "\"  stress=", stress_);
    return UF_NEXT(OnStudent_TakeNext);
}

int UF_Student::SimHours(int hours)
{
    std::this_thread::sleep_for(
        std::chrono::milliseconds(hours * GlobalConfig::kHourMs));
    return hours;
}
