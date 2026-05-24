// ======================================================================
//  load_picker.cpp - flow steps for the A -> B load picker.
// ======================================================================
#include "load_picker.h"

#include "stage.h"

LoadPicker::LoadPicker()
    : uniflow::Uniflow<LoadPicker>("LoadPicker"),
      PickerMotion(GlobalGeometry::kZoneA_mm)
{}

LoadPicker::StepResult LoadPicker::OnLoad_Begin()
{
    Describe("flow start");
    return UF_NEXT(OnLoad_CmdMoveToSource);
}

// --- Source (zone A) ---

LoadPicker::StepResult LoadPicker::OnLoad_CmdMoveToSource()
{
    if (GlobalEnv::Stop()) return Done();
    Describe("cmd: move X to zone A (", GlobalGeometry::kZoneA_mm, " mm)");
    x_axis_.SetTarget(GlobalGeometry::kZoneA_mm);
    return UF_NEXT(OnLoad_WaitAtSource);
}

LoadPicker::StepResult LoadPicker::OnLoad_WaitAtSource()
{
    if (GlobalEnv::Stop()) return Done();
    x_axis_.Update(GlobalGeometry::kXSpeed_mm_per_s);
    Describe("approaching A: ", x_axis_.Position(), " mm");
    if (x_axis_.InPosition()) return UF_NEXT(OnLoad_CmdLowerToPick);
    return Stay();
}

LoadPicker::StepResult LoadPicker::OnLoad_CmdLowerToPick()
{
    if (GlobalEnv::Stop()) return Done();
    Describe("cmd: lower Z to pick");
    z_axis_.SetTarget(GlobalGeometry::kZDown_mm);
    return UF_NEXT(OnLoad_WaitAtPickDown);
}

LoadPicker::StepResult LoadPicker::OnLoad_WaitAtPickDown()
{
    if (GlobalEnv::Stop()) return Done();
    z_axis_.Update(GlobalGeometry::kZSpeed_mm_per_s);
    Describe("lowering to pick: Z=", z_axis_.Position(), " mm");
    if (z_axis_.InPosition()) return UF_NEXT(OnLoad_HandGrip);
    return Stay();
}

LoadPicker::StepResult LoadPicker::OnLoad_HandGrip()
{
    if (GlobalEnv::Stop()) return Done();
    finger_axis_.SetTarget(0.0);
    finger_axis_.Update(GlobalGeometry::kFingerSpeed_mm_per_s);
    Describe("hand: closing gripper (gap=", finger_axis_.Position(), " mm)");
    if (!finger_axis_.InPosition()) return Stay();
    GlobalEnv::ConsumeZoneAPart();
    SetCarrying(true);
    return UF_NEXT(OnLoad_CmdLiftWithPart);
}

LoadPicker::StepResult LoadPicker::OnLoad_CmdLiftWithPart()
{
    if (GlobalEnv::Stop()) return Done();
    Describe("cmd: lift Z with part");
    z_axis_.SetTarget(GlobalGeometry::kZUp_mm);
    return UF_NEXT(OnLoad_WaitAtPickUp);
}

LoadPicker::StepResult LoadPicker::OnLoad_WaitAtPickUp()
{
    if (GlobalEnv::Stop()) return Done();
    z_axis_.Update(GlobalGeometry::kZSpeed_mm_per_s);
    Describe("lifting with part: Z=", z_axis_.Position(), " mm");
    if (z_axis_.InPosition()) return UF_NEXT(OnLoad_CmdMoveToDest);
    return Stay();
}

// --- Destination (zone B) - gated by Stage readiness + partner ---

LoadPicker::StepResult LoadPicker::OnLoad_CmdMoveToDest()
{
    if (GlobalEnv::Stop()) return Done();
    bool may_enter_B =
        Stage::inst().ReadyToReceiveRawPart() && !PartnerInZoneB();
    if (!InsideZoneB() && !may_enter_B)
    {
        // Park at the A-side safety gap. While the axis is still
        // travelling, Stay() so the motion looks like a real axial move
        // (same cadence as the WaitAt* steps); once parked, poll on
        // Wait() so we burn no CPU while gated on an external state.
        x_axis_.SetTarget(GlobalGeometry::kZoneB_mm
                          - GlobalGeometry::kBSafetyGap_mm);
        x_axis_.Update(GlobalGeometry::kXSpeed_mm_per_s);
        if (!x_axis_.InPosition())
        {
            Describe("moving to B-gap: X=", x_axis_.Position(), " mm");
            return Stay();
        }
        Describe("parked at B-gap: stage=",
                 ToString(Stage::inst().state()),
                 " partner_in_B=", PartnerInZoneB());
        return Wait();
    }
    Describe("cmd: move X to zone B (", GlobalGeometry::kZoneB_mm, " mm)");
    x_axis_.SetTarget(GlobalGeometry::kZoneB_mm);
    return UF_NEXT(OnLoad_WaitAtDest);
}

LoadPicker::StepResult LoadPicker::OnLoad_WaitAtDest()
{
    if (GlobalEnv::Stop()) return Done();
    x_axis_.Update(GlobalGeometry::kXSpeed_mm_per_s);
    Describe("approaching B: ", x_axis_.Position(), " mm");
    if (x_axis_.InPosition()) return UF_NEXT(OnLoad_CmdLowerToPlace);
    return Stay();
}

LoadPicker::StepResult LoadPicker::OnLoad_CmdLowerToPlace()
{
    if (GlobalEnv::Stop()) return Done();
    Describe("cmd: lower Z to place");
    z_axis_.SetTarget(GlobalGeometry::kZDown_mm);
    return UF_NEXT(OnLoad_WaitAtPlaceDown);
}

LoadPicker::StepResult LoadPicker::OnLoad_WaitAtPlaceDown()
{
    if (GlobalEnv::Stop()) return Done();
    z_axis_.Update(GlobalGeometry::kZSpeed_mm_per_s);
    Describe("lowering to place: Z=", z_axis_.Position(), " mm");
    if (z_axis_.InPosition()) return UF_NEXT(OnLoad_HandRelease);
    return Stay();
}

LoadPicker::StepResult LoadPicker::OnLoad_HandRelease()
{
    if (GlobalEnv::Stop()) return Done();
    finger_axis_.SetTarget(GlobalGeometry::kFingerOpen_mm);
    finger_axis_.Update(GlobalGeometry::kFingerSpeed_mm_per_s);
    Describe("hand: opening gripper (gap=", finger_axis_.Position(), " mm)");
    if (!finger_axis_.InPosition()) return Stay();
    Stage::inst().OnRawPartReceived();
    SetCarrying(false);
    return UF_NEXT(OnLoad_CmdLiftEmpty);
}

LoadPicker::StepResult LoadPicker::OnLoad_CmdLiftEmpty()
{
    if (GlobalEnv::Stop()) return Done();
    Describe("cmd: lift Z empty");
    z_axis_.SetTarget(GlobalGeometry::kZUp_mm);
    return UF_NEXT(OnLoad_WaitAtPlaceUp);
}

LoadPicker::StepResult LoadPicker::OnLoad_WaitAtPlaceUp()
{
    if (GlobalEnv::Stop()) return Done();
    z_axis_.Update(GlobalGeometry::kZSpeed_mm_per_s);
    Describe("lifting empty: Z=", z_axis_.Position(), " mm");
    if (z_axis_.InPosition()) return UF_NEXT(OnLoad_CmdRetreat);
    return Stay();
}

LoadPicker::StepResult LoadPicker::OnLoad_CmdRetreat()
{
    if (GlobalEnv::Stop()) return Done();
    Describe("cmd: retreat X to A");
    x_axis_.SetTarget(GlobalGeometry::kZoneA_mm);
    return UF_NEXT(OnLoad_WaitAtRetreat);
}

LoadPicker::StepResult LoadPicker::OnLoad_WaitAtRetreat()
{
    if (GlobalEnv::Stop()) return Done();
    x_axis_.Update(GlobalGeometry::kXSpeed_mm_per_s);
    Describe("retreating: X=", x_axis_.Position(), " mm");
    if (x_axis_.InPosition()) { Describe("flow done"); return Done(); }
    return Stay();
}
