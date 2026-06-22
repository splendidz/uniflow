// uf_stage.h - machining cell at zone B. One flow per part, three unit
// operations (Task-Level Syntax): Prepare -> Process -> Cleanup.
//   Prepare : SendStart (async) -> WaitStartAck -> WaitHwReady
//   Process : figure-8 machining run
//   Cleanup : SendCleanup (async) -> WaitCleanupAck -> ReturnToPickPos, then
//             state=ProcessedPartReady, Done.
// Each task is a struct deriving from uniflow::Task<Flow_Stage> that owns its
// steps and the state they share (the settle / run timers, the async command
// workers). OnEnter() re-arms those timers when the task is entered, so a step
// reads elapsed time from the task boundary. HW-persistent state (the table
// axis, the hw-ready latch, state_) lives on the flow and is reached via flow().
#pragma once

#include "globals.h"
#include "motor_io_factory.h"

class Flow_Stage : public uniflow::Uniflow<Flow_Stage>
{
public:
    explicit Flow_Stage(uniflow::Runtime& rt);

    StageState  state()      const { return state_; }
    double      TableX_mm()  const;
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

    // -- Tasks (public so the orchestrator launches them with ctx.StartFlow()). --

    // Prepare : start cmd (async), ack, then wait for HW ready.
    struct Task_Prepare : uniflow::Task<Flow_Stage>
    {
        void OnEnter() override { settle.Restart(); }
        StepResult Entry() override { return Step1_SendStart(); }

    private:
        uniflow::UFTimer settle;                  // hw-ready settle timer

        StepResult Step1_SendStart();
        StepResult Step2_WaitStartAck(uniflow::AsyncId cmd);
        StepResult Step_StartAckTimeout();
        StepResult Step3_WaitHwReady();
        StepResult Step4_HwTimeout();

        static bool SimulateStartCmd();
    } ctx_prepare_;

    // Process : figure-8 machining run over kProcessDuration.
    struct Task_Process : uniflow::Task<Flow_Stage>
    {
        void OnEnter() override { run.Restart(); }
        StepResult Entry() override { return Step1_Process(); }

    private:
        uniflow::UFTimer run;                     // machining elapsed timer

        StepResult Step1_Process();
    } ctx_process_;

    // Cleanup : cleanup cmd (async), ack, return to pick pos.
    struct Task_Cleanup : uniflow::Task<Flow_Stage>
    {
        StepResult Entry() override { return Step1_SendCleanup(); }

    private:
        StepResult Step1_SendCleanup();
        StepResult Step2_WaitCleanupAck(uniflow::AsyncId cmd);
        StepResult Step_CleanupAckTimeout();
        StepResult Step3_ReturnToPickPos();

        static bool SimulateCleanupCmd();
    } ctx_cleanup_;

private:
    static constexpr uniflow::Duration kProcessDuration = 5000ms;

    StageState    state_ = StageState::Idle;

    MotorAxis*    table_x_;
    double        table_y_offset_mm_ = 0.0;
    DigitalLatch* hw_ready_;
};
