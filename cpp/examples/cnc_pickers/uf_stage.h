// stage.h - machining cell at zone B. One flow per part:
//   SendStart (async) -> WaitHwReady -> Processing -> SendCleanup (async)
//   -> MoveToPickPos -> Done, state=ProcessedPartReady.
#pragma once

#include "globals.h"

class UF_Stage : public uniflow::Uniflow<UF_Stage>
{
    UF_UNIFLOW_IMPLEMENT(UF_Stage);

public:
    explicit UF_Stage(uniflow::Runtime& rt)
        : uniflow::Uniflow<UF_Stage>(rt) {}

    StageState  state()      const { return state_; }
    double      TableX_mm()  const { return table_x_axis_.Position(); }
    double      TableY_mm()  const { return table_y_offset_mm_; }

    bool ReadyToReceiveRawPart() const
    {
        return state_ == StageState::Idle;
    }
    bool ReadyToHandOffProcessedPart() const
    {
        return state_ == StageState::ProcessedPartReady;
    }

    void OnRawPartReceived();
    void OnProcessedPartTaken();

    StepResult OnProcess_Begin();

private:
    StepResult OnProcess_SendStartCmd();
    StepResult OnProcess_WaitStartCmdAck();
    StepResult OnProcess_WaitHwReady(uniflow::UFTimer& t);
    StepResult OnProcess_WaitHwReadyTimeout();
    StepResult OnProcess_Run(uniflow::UFTimer& t);
    StepResult OnProcess_SendCleanupCmd();
    StepResult OnProcess_WaitCleanupAck();
    StepResult OnProcess_ReturnToPickPos();

    static bool SimulateStartCmd();
    static bool SimulateCleanupCmd();

    static constexpr uniflow::Duration kProcessDuration = 5000ms;

    StageState  state_ = StageState::Idle;

    MotorAxis   table_x_axis_{GlobalGeometry::kZoneB_mm};
    double      table_y_offset_mm_ = 0.0;
};
