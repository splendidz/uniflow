#include "uf_stage.h"

#include <chrono>
#include <cmath>
#include <thread>

using namespace uniflow;

// The table axis must track the figure-8 trajectory the Process step streams
// to it each round, so it is given a high command speed - effectively direct
// position control rather than a slow point-to-point move.
static constexpr double kTableSpeed_mm_per_s = 5000.0;

Flow_Stage::Flow_Stage(uniflow::Runtime& rt)
    : uniflow::Uniflow<Flow_Stage>(rt, "Flow_Stage"),
      table_x_(MotorIOFactory::inst().CreateAxis(
          "stage_table_x", GlobalGeometry::kZoneB_mm, kTableSpeed_mm_per_s)),
      hw_ready_(MotorIOFactory::inst().CreateLatch("stage_hw_ready", 0.2, 0.7))
{
    AddTask(ctx_prepare_);
    AddTask(ctx_process_);
    AddTask(ctx_cleanup_);
}

double Flow_Stage::TableX_mm() const
{
    return table_x_->Position();
}

void Flow_Stage::OnRawPartReceived()
{
    state_ = StageState::RawPartLoaded;
    Describe("raw part loaded");
}

void Flow_Stage::OnProcessedPartTaken()
{
    state_ = StageState::Idle;
    Describe("empty");
}

// ======================================================================
//  Task: Prepare
// ======================================================================

StepResult Flow_Stage::Task_Prepare::Step1_SendStart()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    // Guarded bootstrap: only a freshly loaded raw part may begin processing
    // (the orchestrator should only launch Prepare in that state).
    if (flow().state_ != StageState::RawPartLoaded)
    {
        return Fail();
    }
    Describe("send start cmd");
    // SubmitAsync returns the job's AsyncId; carry it to the step that reads
    // the result. id 0 means the submission was rejected (in-flight cap).
    AsyncId cmd = SubmitAsync(UF_FN(SimulateStartCmd));
    if (cmd == 0)
    {
        Describe("start cmd rejected");
        return Fail();
    }
    return Next(UF_FN(Step2_WaitStartAck), cmd);
}

StepResult Flow_Stage::Task_Prepare::Step2_WaitStartAck(uniflow::AsyncId cmd)
{
    // The worker is no longer awaited by a gate - we poll its slot by id. While
    // it is still in flight, keep polling; if the ack never lands within 2s,
    // route to the timeout step (which abandons the worker via ClearAsync).
    auto r = AsyncResult<bool>(cmd);
    if (r.pending())
    {
        Describe("wait start ack");
        return StayUntil(2000ms, UF_FN(Step_StartAckTimeout));
    }
    if (!r.ok() || !*r.return_value)
    {
        Describe("start cmd failed");
        return Fail();
    }
    Describe("wait hw ready");
    flow().hw_ready_->Arm();
    return Next(UF_FN(Step3_WaitHwReady));
}

StepResult Flow_Stage::Task_Prepare::Step_StartAckTimeout()
{
    // The start command never acknowledged. Abandon the in-flight worker
    // (OnAsyncAbandoned fires) and fail the flow.
    Describe("start ack timeout");
    ClearAsync();
    return Fail();
}

StepResult Flow_Stage::Task_Prepare::Step3_WaitHwReady()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }

    // HeldFor proceeds once the ready flag has held STABLE for 50ms (a settling
    // handshake), not on the first transient high. settle was re-armed by
    // OnEnter on task entry, so its sustain accumulator spans the Stay re-entries.
    if (settle.HeldFor(flow().hw_ready_->IsReady(), 50ms))
    {
        // Prepare task done: hardware is ready. The orchestrator launches the
        // Process task next (state-driven).
        flow().state_ = StageState::Prepared;
        Describe("prepared");
        return Done();
    }
    // Still settling: keep polling, but if ready never settles within 3s,
    // route to the timeout step (the step-level "catch") rather than poll
    // forever. The deadline is measured from step entry, not from this call.
    Describe("wait hw ready");
    return StayUntil(3000ms, UF_FN(Step4_HwTimeout));
}

StepResult Flow_Stage::Task_Prepare::Step4_HwTimeout()
{
    Describe("hw ready timeout");
    return Fail();
}

bool Flow_Stage::Task_Prepare::SimulateStartCmd()
{
    std::this_thread::sleep_for(300ms);
    return true;
}

// ======================================================================
//  Task: Process
// ======================================================================

StepResult Flow_Stage::Task_Process::Step1_Process()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    // run was re-armed by OnEnter on task entry; Stay re-entries touch the same
    // timer, so Elapsed() grows from the task's entry.
    auto   elapsed = run.Elapsed();
    double frac =
        std::chrono::duration<double>(elapsed).count()
        / std::chrono::duration<double>(kProcessDuration).count();
    if (frac > 1.0)
    {
        frac = 1.0;
    }

    // Figure-8 (Gerono lemniscate) machining pattern:
    //   x(t) = A_x * sin(t)
    //   y(t) = A_y * sin(2t)
    // The 2:1 frequency ratio is what produces the '8' crossing at zone
    // centre. Four full loops over the process duration keep it lively
    // without the y axis dominating the motion.
    constexpr double kTau   = 6.2831853071795864;
    constexpr int    kLoops = 4;
    constexpr double kAmpY  = 30.0;        // mm; ~A_x / 2.5 for a flat 8
    double phase    = frac * kTau * kLoops;
    double sweep_mm = GlobalGeometry::kStageTravel_mm * std::sin(phase);
    flow().table_x_->Move(GlobalGeometry::kZoneB_mm + sweep_mm);
    flow().table_y_offset_mm_ = kAmpY * std::sin(phase * 2.0);

    if (elapsed >= kProcessDuration)
    {
        // Process task done: machining finished. The orchestrator launches the
        // Cleanup task next.
        flow().state_ = StageState::Machined;
        Describe("machined");
        return Done();
    }
    Describe("processing");
    return Stay();
}

// ======================================================================
//  Task: Cleanup
// ======================================================================

StepResult Flow_Stage::Task_Cleanup::Step1_SendCleanup()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    Describe("send cleanup cmd");
    AsyncId cmd = SubmitAsync(UF_FN(SimulateCleanupCmd));
    if (cmd == 0)
    {
        Describe("cleanup cmd rejected");
        return Fail();
    }
    return Next(UF_FN(Step2_WaitCleanupAck), cmd);
}

StepResult Flow_Stage::Task_Cleanup::Step2_WaitCleanupAck(uniflow::AsyncId cmd)
{
    auto r = AsyncResult<bool>(cmd);
    if (r.pending())
    {
        Describe("wait cleanup ack");
        return StayUntil(2000ms, UF_FN(Step_CleanupAckTimeout));
    }
    if (!r.ok() || !*r.return_value)
    {
        Describe("cleanup failed");
        return Fail();
    }
    Describe("return to pick pos");
    flow().table_x_->Move(GlobalGeometry::kZoneB_mm);
    return Next(UF_FN(Step3_ReturnToPickPos));
}

StepResult Flow_Stage::Task_Cleanup::Step_CleanupAckTimeout()
{
    Describe("cleanup ack timeout");
    ClearAsync();
    return Fail();
}

StepResult Flow_Stage::Task_Cleanup::Step3_ReturnToPickPos()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    flow().table_y_offset_mm_ = 0.0;
    if (!flow().table_x_->InPosition())
    {
        return Stay();
    }

    flow().state_ = StageState::ProcessedPartReady;
    Describe("ready to hand off");
    return Done();
}

bool Flow_Stage::Task_Cleanup::SimulateCleanupCmd()
{
    std::this_thread::sleep_for(200ms);
    return true;
}
