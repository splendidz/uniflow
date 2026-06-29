// uf_unload_picker.h - carries the finished part B -> C. Same shape as
// Flow_LoadPicker (Task-Level Syntax: Pick -> Place), but the SOURCE is the
// contested B zone. Each task is a struct deriving from uniflow::Task<...> that
// owns its steps; the flow holds one instance and steps reach it through flow().
// PartnerInZoneB for both pickers is defined in uf_unload_picker.cpp, where both
// flow classes are visible.
#pragma once

#include "globals.h"
#include "motor_io_factory.h"

class Flow_UnloadPicker : public uniflow::Uniflow<Flow_UnloadPicker>
{
public:
    explicit Flow_UnloadPicker(uniflow::Runtime& rt);

    // Motion state read by peers and by the visualization snapshot.
    double X_mm()         const;
    double Z_mm()         const;
    double FingerGap_mm() const;
    bool   Carrying()     const;
    bool   InsideZoneB()  const;

    // -- Tasks (public so the orchestrator launches them with task.StartFlow()). --

    // Task: Pick (zone B, contested). Enter B only when Flow_Stage has started
    // processing AND Flow_LoadPicker is neither in B nor carrying toward it;
    // then hover with Z up until hand-off readiness.
    struct Task_Pick : uniflow::Task<Flow_UnloadPicker>
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

    // Task: Place (zone C).
    struct Task_Place : uniflow::Task<Flow_UnloadPicker>
    {
        StepResult Entry() override { return Step1_CmdMoveToUnload(); }

    private:
        StepResult Step1_CmdMoveToUnload();
        StepResult Step2_WaitAtUnload();
        StepResult Step3_CmdLowerToPlace();
        StepResult Step4_WaitAtPlaceDown();
        StepResult Step5_HandRelease();
        StepResult Step6_CmdLiftEmpty();
        StepResult Step7_WaitAtPlaceUp();
        StepResult Step8_CmdRetreat();
        StepResult Step9_WaitAtRetreat();
    } task_place_;

private:
    bool PartnerInZoneB() const;

    MotorAxis* x_;
    MotorAxis* z_;
    MotorAxis* finger_;
    bool       carrying_ = false;
};
