// ======================================================================
//  unload_picker.cpp - flow steps for the B -> C unload picker,
//  plus the cross-picker queries (both class definitions are visible
//  here once load_picker.h is included).
// ======================================================================
#include "unload_picker.h"

#include "load_picker.h"
#include "stage.h"

UnloadPicker::UnloadPicker()
    : uniflow::Uniflow<UnloadPicker>("UnloadPicker"),
      PickerMotion(GlobalGeometry::kZoneC_mm)
{}

UnloadPicker::StepResult UnloadPicker::OnUnload_Begin()
{
    Describe("flow start");
    return UF_NEXT(OnUnload_CmdMoveToSource);
}

// --- Source (zone B) - gated by Stage hand-off readiness + partner ---

UnloadPicker::StepResult UnloadPicker::OnUnload_CmdMoveToSource()
{
    if (GlobalEnv::Stop()) return Done();
    bool may_enter_B =
        Stage::inst().ReadyToHandOffProcessedPart() && !PartnerInZoneB();
    if (!InsideZoneB() && !may_enter_B)
    {
        // Park at the C-side safety gap. Stay() while the axis is still
        // travelling so the prefetch motion looks smooth (matches the
        // WaitAt* pattern); once parked, poll on Wait() so we burn no
        // CPU while gated on the stage finishing.
        x_axis_.SetTarget(GlobalGeometry::kZoneB_mm
                          + GlobalGeometry::kBSafetyGap_mm);
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
    return UF_NEXT(OnUnload_WaitAtSource);
}

UnloadPicker::StepResult UnloadPicker::OnUnload_WaitAtSource()
{
    if (GlobalEnv::Stop()) return Done();
    x_axis_.Update(GlobalGeometry::kXSpeed_mm_per_s);
    Describe("approaching B: ", x_axis_.Position(), " mm");
    if (x_axis_.InPosition()) return UF_NEXT(OnUnload_CmdLowerToPick);
    return Stay();
}

UnloadPicker::StepResult UnloadPicker::OnUnload_CmdLowerToPick()
{
    if (GlobalEnv::Stop()) return Done();
    Describe("cmd: lower Z to pick");
    z_axis_.SetTarget(GlobalGeometry::kZDown_mm);
    return UF_NEXT(OnUnload_WaitAtPickDown);
}

UnloadPicker::StepResult UnloadPicker::OnUnload_WaitAtPickDown()
{
    if (GlobalEnv::Stop()) return Done();
    z_axis_.Update(GlobalGeometry::kZSpeed_mm_per_s);
    Describe("lowering to pick: Z=", z_axis_.Position(), " mm");
    if (z_axis_.InPosition()) return UF_NEXT(OnUnload_HandGrip);
    return Stay();
}

UnloadPicker::StepResult UnloadPicker::OnUnload_HandGrip()
{
    if (GlobalEnv::Stop()) return Done();
    finger_axis_.SetTarget(0.0);
    finger_axis_.Update(GlobalGeometry::kFingerSpeed_mm_per_s);
    Describe("hand: closing gripper (gap=", finger_axis_.Position(), " mm)");
    if (!finger_axis_.InPosition()) return Stay();
    Stage::inst().OnProcessedPartTaken();
    SetCarrying(true);
    return UF_NEXT(OnUnload_CmdLiftWithPart);
}

UnloadPicker::StepResult UnloadPicker::OnUnload_CmdLiftWithPart()
{
    if (GlobalEnv::Stop()) return Done();
    Describe("cmd: lift Z with part");
    z_axis_.SetTarget(GlobalGeometry::kZUp_mm);
    return UF_NEXT(OnUnload_WaitAtPickUp);
}

UnloadPicker::StepResult UnloadPicker::OnUnload_WaitAtPickUp()
{
    if (GlobalEnv::Stop()) return Done();
    z_axis_.Update(GlobalGeometry::kZSpeed_mm_per_s);
    Describe("lifting with part: Z=", z_axis_.Position(), " mm");
    if (z_axis_.InPosition()) return UF_NEXT(OnUnload_CmdMoveToUnload);
    return Stay();
}

// --- Destination (zone C) ---

UnloadPicker::StepResult UnloadPicker::OnUnload_CmdMoveToUnload()
{
    if (GlobalEnv::Stop()) return Done();
    Describe("cmd: move X to zone C (", GlobalGeometry::kZoneC_mm, " mm)");
    x_axis_.SetTarget(GlobalGeometry::kZoneC_mm);
    return UF_NEXT(OnUnload_WaitAtUnload);
}

UnloadPicker::StepResult UnloadPicker::OnUnload_WaitAtUnload()
{
    if (GlobalEnv::Stop()) return Done();
    x_axis_.Update(GlobalGeometry::kXSpeed_mm_per_s);
    Describe("approaching C: ", x_axis_.Position(), " mm");
    if (x_axis_.InPosition()) return UF_NEXT(OnUnload_CmdLowerToPlace);
    return Stay();
}

UnloadPicker::StepResult UnloadPicker::OnUnload_CmdLowerToPlace()
{
    if (GlobalEnv::Stop()) return Done();
    Describe("cmd: lower Z to place");
    z_axis_.SetTarget(GlobalGeometry::kZDown_mm);
    return UF_NEXT(OnUnload_WaitAtPlaceDown);
}

UnloadPicker::StepResult UnloadPicker::OnUnload_WaitAtPlaceDown()
{
    if (GlobalEnv::Stop()) return Done();
    z_axis_.Update(GlobalGeometry::kZSpeed_mm_per_s);
    Describe("lowering to place: Z=", z_axis_.Position(), " mm");
    if (z_axis_.InPosition()) return UF_NEXT(OnUnload_HandRelease);
    return Stay();
}

UnloadPicker::StepResult UnloadPicker::OnUnload_HandRelease()
{
    if (GlobalEnv::Stop()) return Done();
    finger_axis_.SetTarget(GlobalGeometry::kFingerOpen_mm);
    finger_axis_.Update(GlobalGeometry::kFingerSpeed_mm_per_s);
    Describe("hand: opening gripper (gap=", finger_axis_.Position(), " mm)");
    if (!finger_axis_.InPosition()) return Stay();
    GlobalEnv::IncDelivered();
    SetCarrying(false);
    return UF_NEXT(OnUnload_CmdLiftEmpty);
}

UnloadPicker::StepResult UnloadPicker::OnUnload_CmdLiftEmpty()
{
    if (GlobalEnv::Stop()) return Done();
    Describe("cmd: lift Z empty");
    z_axis_.SetTarget(GlobalGeometry::kZUp_mm);
    return UF_NEXT(OnUnload_WaitAtPlaceUp);
}

UnloadPicker::StepResult UnloadPicker::OnUnload_WaitAtPlaceUp()
{
    if (GlobalEnv::Stop()) return Done();
    z_axis_.Update(GlobalGeometry::kZSpeed_mm_per_s);
    Describe("lifting empty: Z=", z_axis_.Position(), " mm");
    if (z_axis_.InPosition()) return UF_NEXT(OnUnload_CmdRetreat);
    return Stay();
}

UnloadPicker::StepResult UnloadPicker::OnUnload_CmdRetreat()
{
    if (GlobalEnv::Stop()) return Done();
    Describe("cmd: retreat X to C");
    x_axis_.SetTarget(GlobalGeometry::kZoneC_mm);
    return UF_NEXT(OnUnload_WaitAtRetreat);
}

UnloadPicker::StepResult UnloadPicker::OnUnload_WaitAtRetreat()
{
    if (GlobalEnv::Stop()) return Done();
    x_axis_.Update(GlobalGeometry::kXSpeed_mm_per_s);
    Describe("retreating: X=", x_axis_.Position(), " mm");
    if (x_axis_.InPosition()) { Describe("flow done"); return Done(); }
    return Stay();
}

// ----- cross-picker queries -------------------------------------------

bool LoadPicker::PartnerInZoneB() const
{
    return UnloadPicker::GetInst().InsideZoneB();
}
bool UnloadPicker::PartnerInZoneB() const
{
    return LoadPicker::GetInst().InsideZoneB();
}
