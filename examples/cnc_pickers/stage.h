// ======================================================================
//  stage.h - the machining cell at zone B. Single instance.
//
//  Lifecycle is a sequence of flows started by the Orchestrator. One
//  flow == one part processed. The flow runs:
//
//    SendStartHwCommand (async, ~1 s)
//    WaitHwReady        (poll, ~1 s)
//    Processing         (~5 s, time-based)
//    SendCleanupCommand (async, ~0.5 s)
//    MoveToPickPos                 - retract table, InPosition wait
//    -> Done; state becomes ProcessedPartReady
//
//  Pickers call ReadyToReceiveRawPart / ReadyToHandOffProcessedPart and
//  never touch the Stage's internal phase.
//
//  Step bodies are defined in stage.cpp.
// ======================================================================
#pragma once

#include "globals.h"

class Stage : public uniflow::Uniflow<Stage>
{
    UF_SINGLETON(Stage);
    //
    // === SINGLE INSTANCE: exactly one Stage cell at zone B. ===
    //
    // UF_SINGLETON above is not boilerplate - it encodes a hardware
    // assumption. A second instance would be a logic error and the macro
    // prevents it at compile time.
    //

public:
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
