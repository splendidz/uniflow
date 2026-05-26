#include "uf_stage.h"

#include <chrono>
#include <cmath>
#include <thread>

void UF_Stage::OnRawPartReceived()
{
    state_ = StageState::RawPartLoaded;
    Describe("raw part loaded");
}

void UF_Stage::OnProcessedPartTaken()
{
    state_ = StageState::Idle;
    Describe("empty");
}

UF_Stage::StepResult UF_Stage::OnProcess_Begin()
{
    if (state_ != StageState::RawPartLoaded)
        return Fail();
    state_ = StageState::Processing;
    Describe("send start cmd");
    return UF_NEXT(OnProcess_SendStartCmd);
}

UF_Stage::StepResult UF_Stage::OnProcess_SendStartCmd()
{
    if (GlobalEnv::Stop()) return Done();
    UF_ASYNC(SimulateStartCmd);
    return UF_NEXT(OnProcess_WaitStartCmdAck);
}

UF_Stage::StepResult UF_Stage::OnProcess_WaitStartCmdAck()
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

UF_Stage::StepResult UF_Stage::OnProcess_WaitHwReady(uniflow::UFTimer& t)
{
    if (GlobalEnv::Stop()) return Done();

    auto result = t.OnWait(HwSimulator::IsReady(), 3000ms);
    if (result == uniflow::UFTimer::Timeout)
    {
        Describe("hw ready timeout");
        return Fail();
    }
    else if (result == uniflow::UFTimer::Waiting)
        return Stay();

    Describe("processing");
    table_y_offset_mm_ = 0.0;
    table_x_axis_.SetTarget(GlobalGeometry::kZoneB_mm
                            + GlobalGeometry::kStageTravel_mm);
    return UF_NEXT(OnProcess_Run, uniflow::UFTimer{});
}

UF_Stage::StepResult UF_Stage::OnProcess_Run(uniflow::UFTimer& t)
{
    if (GlobalEnv::Stop()) return Done();
    // by-ref param: Stay re-enter touches the SAME timer in the next-step
    // capture, so Elapsed() keeps growing from initial arming.
    auto   elapsed = t.Elapsed();
    double frac =
        std::chrono::duration<double>(elapsed).count()
        / std::chrono::duration<double>(kProcessDuration).count();
    if (frac > 1.0) frac = 1.0;

    // Figure-8 (Gerono lemniscate) machining pattern:
    //   x(t) = A_x * sin(t)
    //   y(t) = A_y * sin(2t)
    // The 2:1 frequency ratio is what produces the '8' crossing at zone
    // centre. Four full loops over the process duration keep it lively
    // without the y axis dominating the motion the way the layered
    // sinusoids did before.
    constexpr double kTau    = 6.2831853071795864;
    constexpr int    kLoops  = 4;
    constexpr double kAmpY   = 30.0;        // mm; ~A_x / 2.5 for a flat 8
    double phase = frac * kTau * kLoops;
    double sweep_mm = GlobalGeometry::kStageTravel_mm * std::sin(phase);
    table_x_axis_.SetTarget(GlobalGeometry::kZoneB_mm + sweep_mm);
    table_y_offset_mm_ = kAmpY * std::sin(phase * 2.0);
    table_x_axis_.Update(GlobalGeometry::kXSpeed_mm_per_s * 1.5);

    if (elapsed >= kProcessDuration)
    {
        Describe("send cleanup cmd");
        return UF_NEXT(OnProcess_SendCleanupCmd);
    }
    return Stay();
}

UF_Stage::StepResult UF_Stage::OnProcess_SendCleanupCmd()
{
    if (GlobalEnv::Stop()) return Done();
    UF_ASYNC(SimulateCleanupCmd);
    return UF_NEXT(OnProcess_WaitCleanupAck);
}

UF_Stage::StepResult UF_Stage::OnProcess_WaitCleanupAck()
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

UF_Stage::StepResult UF_Stage::OnProcess_ReturnToPickPos()
{
    if (GlobalEnv::Stop()) return Done();
    table_x_axis_.Update(GlobalGeometry::kXSpeed_mm_per_s);
    table_y_offset_mm_ = 0.0;
    if (!table_x_axis_.InPosition())
        return Stay();

    state_ = StageState::ProcessedPartReady;
    Describe("ready to hand off");
    return Done();
}

bool UF_Stage::SimulateStartCmd()
{
    std::this_thread::sleep_for(300ms);
    return true;
}

bool UF_Stage::SimulateCleanupCmd()
{
    std::this_thread::sleep_for(200ms);
    return true;
}
