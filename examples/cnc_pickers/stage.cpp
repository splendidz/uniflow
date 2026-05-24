// ======================================================================
//  stage.cpp - Stage flow steps and HW handshake workers.
// ======================================================================
#include "stage.h"

#include <chrono>
#include <cmath>
#include <thread>

void Stage::OnRawPartReceived()
{
    state_ = StageState::RawPartLoaded;
    Describe("raw part loaded");
    // Wake the Orchestrator so TryStartStageProcessing runs without
    // waiting out the next kSchedTick.
    uniflow::NotifyAll();
}

void Stage::OnProcessedPartTaken()
{
    state_ = StageState::Idle;
    Describe("empty");
    uniflow::NotifyAll();
}

Stage::StepResult Stage::OnProcess_Begin()
{
    if (state_ != StageState::RawPartLoaded)
        return Fail();
    state_ = StageState::Processing;
    Describe("send start cmd");
    return UF_NEXT(OnProcess_SendStartCmd);
}

Stage::StepResult Stage::OnProcess_SendStartCmd()
{
    if (GlobalEnv::Stop()) return Done();
    UF_ASYNC(SimulateStartCmd);
    return UF_NEXT(OnProcess_WaitStartCmdAck);
}

Stage::StepResult Stage::OnProcess_WaitStartCmdAck()
{
    auto r = AsyncResult<bool>();
    if (r.failed() || r.is_timeout() || !r.value())
    {
        Describe("start cmd failed");
        return Fail();
    }
    Describe("wait hw ready");
    HwSimulator::DoReady();
    return UF_NEXT(OnProcess_WaitHwReady, uniflow::UFTimer{});
}

Stage::StepResult Stage::OnProcess_WaitHwReady(uniflow::UFTimer& t)
{
    if (GlobalEnv::Stop()) return Done();

    auto result = t.OnWait(HwSimulator::IsReady(), 3000ms);
    if (result == uniflow::UFTimer::Timeout)
    {
        Describe("hw ready timeout");
        return Fail();
    }
    else if (result == uniflow::UFTimer::Waiting)
        return Wait();

    Describe("processing");
    table_y_offset_mm_ = 0.0;
    table_x_axis_.SetTarget(GlobalGeometry::kZoneB_mm
                            + GlobalGeometry::kStageTravel_mm);
    return UF_NEXT(OnProcess_Run, uniflow::UFTimer{});
}

Stage::StepResult Stage::OnProcess_Run(uniflow::UFTimer& t)
{
    if (GlobalEnv::Stop()) return Done();
    // Stay() / Wait() re-enter THIS step fn: by-ref signature means we
    // touch the SAME UFTimer instance stored in the next-step capture
    // tuple every time, so Elapsed() keeps growing from the moment the
    // step was first armed.
    auto   elapsed = t.Elapsed();
    double frac =
        std::chrono::duration<double>(elapsed).count()
        / std::chrono::duration<double>(kProcessDuration).count();
    if (frac > 1.0) frac = 1.0;
    double sweep_mm =
        GlobalGeometry::kStageTravel_mm * std::sin(frac * 6.283 * 2.0);
    table_x_axis_.SetTarget(GlobalGeometry::kZoneB_mm + sweep_mm);
    table_y_offset_mm_ = 40.0 * std::sin(frac * 6.283 * 3.0);
    table_x_axis_.Update(GlobalGeometry::kXSpeed_mm_per_s);

    if (elapsed >= kProcessDuration)
    {
        Describe("send cleanup cmd");
        return UF_NEXT(OnProcess_SendCleanupCmd);
    }
    return Wait(GlobalTiming::kPoll_fast);
}

Stage::StepResult Stage::OnProcess_SendCleanupCmd()
{
    if (GlobalEnv::Stop()) return Done();
    UF_ASYNC(SimulateCleanupCmd);
    return UF_NEXT(OnProcess_WaitCleanupAck);
}

Stage::StepResult Stage::OnProcess_WaitCleanupAck()
{
    auto r = AsyncResult<bool>();
    if (r.failed() || r.is_timeout() || !r.value())
    {
        Describe("cleanup failed");
        return Fail();
    }
    Describe("return to pick pos");
    table_x_axis_.SetTarget(GlobalGeometry::kZoneB_mm);
    return UF_NEXT(OnProcess_ReturnToPickPos);
}

Stage::StepResult Stage::OnProcess_ReturnToPickPos()
{
    if (GlobalEnv::Stop()) return Done();
    table_x_axis_.Update(GlobalGeometry::kXSpeed_mm_per_s);
    table_y_offset_mm_ = 0.0;
    if (!table_x_axis_.InPosition())
        return Wait(GlobalTiming::kPoll_fast);

    state_ = StageState::ProcessedPartReady;
    Describe("ready to hand off");
    // Wake the Orchestrator so TryStartUnloadPicker runs immediately.
    uniflow::NotifyAll();
    return Done();
}

bool Stage::SimulateStartCmd()
{
    std::this_thread::sleep_for(300ms);
    return true;
}

bool Stage::SimulateCleanupCmd()
{
    std::this_thread::sleep_for(200ms);
    return true;
}
