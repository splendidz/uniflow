// uf_load_picker.h - carries a raw part A -> B. One flow, two unit operations
// (Task-Level Syntax): Pick (zone A) -> Place (zone B).
//
// Each task is a struct deriving from uniflow::Task<Flow_LoadPicker>: it owns
// the state its steps share AND the step member functions. The flow holds one
// instance of each (task_pick_, task_place_), declared public so the orchestrator
// can launch it with task.StartFlow(). A step reaches the flow's borrowed axes
// and peers through flow(); Entry() names the task's first step.
//
// The flow owns the motor axes it uses as pointers handed out by the
// MotorIOFactory singleton; the factory thread integrates them. Steps only
// command (Move) and poll (InPosition) - they never step the motor themselves.
#pragma once

#include "globals.h"
#include "motor_io_factory.h"

class Flow_LoadPicker : public uniflow::Uniflow<Flow_LoadPicker>
{
public:
    explicit Flow_LoadPicker(uniflow::Runtime& rt);

    // Motion state read by peers (the unload picker's B-zone gate) and by the
    // visualization snapshot.
    double X_mm()         const;
    double Z_mm()         const;
    double FingerGap_mm() const;
    bool   Carrying()     const;
    bool   InsideZoneB()  const;

    // -- Tasks (public so the orchestrator launches them with task.StartFlow()).
    //    Each owns its steps; steps reach the flow through flow(). --

    // Task: Pick (zone A) - approach, lower, grip, lift, then go idle.
    struct Task_Pick : uniflow::Task<Flow_LoadPicker>
    {
        StepResult Entry() override { return Step1_CmdMoveToSource(); }

    private:
        StepResult Step1_CmdMoveToSource();
        StepResult Step2_WaitAtSource();
        StepResult Step3_CmdLowerToPick();
        StepResult Step4_WaitAtPickDown();
        StepResult Step5_HandGrip();
        StepResult Step6_CmdLiftWithPart();
        StepResult Step7_WaitAtPickUp();
    } task_pick_;

    // Task: Place (zone B) - gated by Flow_Stage readiness + partner.
    struct Task_Place : uniflow::Task<Flow_LoadPicker>
    {
        StepResult Entry() override { return Step1_CmdMoveToDest(); }

    private:
        StepResult Step1_CmdMoveToDest();
        StepResult Step2_WaitAtDest();
        StepResult Step3_CmdLowerToPlace();
        StepResult Step4_WaitAtPlaceDown();
        StepResult Step5_HandRelease();
        StepResult Step6_CmdLiftEmpty();
        StepResult Step7_WaitAtPlaceUp();
        StepResult Step8_CmdRetreat();
        StepResult Step9_WaitAtRetreat();
    } task_place_;

private:
    // Defined out-of-line in uf_unload_picker.cpp once Flow_UnloadPicker is
    // visible. Declaration only.
    bool PartnerInZoneB() const;

    MotorAxis* x_;
    MotorAxis* z_;
    MotorAxis* finger_;
    bool       carrying_ = false;
};
