#include "load_picker.h"

#include "app.h"
#include "stage.h"

LoadPicker::StepResult LoadPicker::OnLoad_Begin()
{
    Describe("flow start");
    return UF_NEXT(OnLoad_CmdMoveToSource);
}

// --- Source (zone A) ---

LoadPicker::StepResult LoadPicker::OnLoad_CmdMoveToSource()
{
    if (GlobalEnv::Stop()) return Done();
    Describe("cmd: move to zone A");
    x_axis_.SetTarget(GlobalGeometry::kZoneA_mm);
    return UF_NEXT(OnLoad_WaitAtSource);
}

LoadPicker::StepResult LoadPicker::OnLoad_WaitAtSource()
{
    if (GlobalEnv::Stop()) return Done();
    x_axis_.Update(GlobalGeometry::kXSpeed_mm_per_s);
    Describe("approaching A");
    if (x_axis_.InPosition()) return UF_NEXT(OnLoad_CmdLowerToPick);
    return Stay();
}

LoadPicker::StepResult LoadPicker::OnLoad_CmdLowerToPick()
{
    if (GlobalEnv::Stop()) return Done();
    Describe("cmd: lower to pick");
    z_axis_.SetTarget(GlobalGeometry::kZDown_mm);
    return UF_NEXT(OnLoad_WaitAtPickDown);
}

LoadPicker::StepResult LoadPicker::OnLoad_WaitAtPickDown()
{
    if (GlobalEnv::Stop()) return Done();
    z_axis_.Update(GlobalGeometry::kZSpeed_mm_per_s);
    Describe("lowering to pick");
    if (z_axis_.InPosition()) return UF_NEXT(OnLoad_HandGrip);
    return Stay();
}

LoadPicker::StepResult LoadPicker::OnLoad_HandGrip()
{
    if (GlobalEnv::Stop()) return Done();
    finger_axis_.SetTarget(0.0);
    finger_axis_.Update(GlobalGeometry::kFingerSpeed_mm_per_s);
    Describe("closing gripper");
    if (!finger_axis_.InPosition()) return Stay();
    GlobalEnv::ConsumeZoneAPart();
    SetCarrying(true);
    return UF_NEXT(OnLoad_CmdLiftWithPart);
}

LoadPicker::StepResult LoadPicker::OnLoad_CmdLiftWithPart()
{
    if (GlobalEnv::Stop()) return Done();
    Describe("cmd: lift with part");
    z_axis_.SetTarget(GlobalGeometry::kZUp_mm);
    return UF_NEXT(OnLoad_WaitAtPickUp);
}

LoadPicker::StepResult LoadPicker::OnLoad_WaitAtPickUp()
{
    if (GlobalEnv::Stop()) return Done();
    z_axis_.Update(GlobalGeometry::kZSpeed_mm_per_s);
    Describe("lifting with part");
    if (z_axis_.InPosition()) return UF_NEXT(OnLoad_CmdMoveToDest);
    return Stay();
}

// --- Destination (zone B) - gated by Stage readiness + partner ---

LoadPicker::StepResult LoadPicker::OnLoad_CmdMoveToDest()
{
    if (GlobalEnv::Stop()) return Done();
    bool may_enter_B =
        App::inst().stage.ReadyToReceiveRawPart() && !PartnerInZoneB();
    if (!InsideZoneB() && !may_enter_B)
    {
        x_axis_.SetTarget(GlobalGeometry::kZoneB_mm
                          - GlobalGeometry::kBSafetyGap_mm);
        x_axis_.Update(GlobalGeometry::kXSpeed_mm_per_s);
        if (!x_axis_.InPosition())
        {
            Describe("moving to A-gap");
            return Stay();
        }
        Describe("parked at A-gap: stage=", ToString(App::inst().stage.state()),
                 " partner_in_B=", PartnerInZoneB());
        return Stay();
    }
    Describe("cmd: move to zone B");
    x_axis_.SetTarget(GlobalGeometry::kZoneB_mm);
    return UF_NEXT(OnLoad_WaitAtDest);
}

LoadPicker::StepResult LoadPicker::OnLoad_WaitAtDest()
{
    if (GlobalEnv::Stop()) return Done();
    x_axis_.Update(GlobalGeometry::kXSpeed_mm_per_s);
    Describe("approaching B");
    if (x_axis_.InPosition()) return UF_NEXT(OnLoad_CmdLowerToPlace);
    return Stay();
}

LoadPicker::StepResult LoadPicker::OnLoad_CmdLowerToPlace()
{
    if (GlobalEnv::Stop()) return Done();
    Describe("cmd: lower to place");
    z_axis_.SetTarget(GlobalGeometry::kZDown_mm);
    return UF_NEXT(OnLoad_WaitAtPlaceDown);
}

LoadPicker::StepResult LoadPicker::OnLoad_WaitAtPlaceDown()
{
    if (GlobalEnv::Stop()) return Done();
    z_axis_.Update(GlobalGeometry::kZSpeed_mm_per_s);
    Describe("lowering to place");
    if (z_axis_.InPosition()) return UF_NEXT(OnLoad_HandRelease);
    return Stay();
}

LoadPicker::StepResult LoadPicker::OnLoad_HandRelease()
{
    if (GlobalEnv::Stop()) return Done();
    finger_axis_.SetTarget(GlobalGeometry::kFingerOpen_mm);
    finger_axis_.Update(GlobalGeometry::kFingerSpeed_mm_per_s);
    Describe("opening gripper");
    if (!finger_axis_.InPosition()) return Stay();
    App::inst().stage.OnRawPartReceived();
    SetCarrying(false);
    return UF_NEXT(OnLoad_CmdLiftEmpty);
}

LoadPicker::StepResult LoadPicker::OnLoad_CmdLiftEmpty()
{
    if (GlobalEnv::Stop()) return Done();
    Describe("cmd: lift empty");
    z_axis_.SetTarget(GlobalGeometry::kZUp_mm);
    return UF_NEXT(OnLoad_WaitAtPlaceUp);
}

LoadPicker::StepResult LoadPicker::OnLoad_WaitAtPlaceUp()
{
    if (GlobalEnv::Stop()) return Done();
    z_axis_.Update(GlobalGeometry::kZSpeed_mm_per_s);
    Describe("lifting empty");
    if (z_axis_.InPosition()) return UF_NEXT(OnLoad_CmdRetreat);
    return Stay();
}

LoadPicker::StepResult LoadPicker::OnLoad_CmdRetreat()
{
    if (GlobalEnv::Stop()) return Done();
    Describe("cmd: retreat to A");
    x_axis_.SetTarget(GlobalGeometry::kZoneA_mm);
    return UF_NEXT(OnLoad_WaitAtRetreat);
}

LoadPicker::StepResult LoadPicker::OnLoad_WaitAtRetreat()
{
    if (GlobalEnv::Stop()) return Done();
    x_axis_.Update(GlobalGeometry::kXSpeed_mm_per_s);
    Describe("retreating");
    if (x_axis_.InPosition()) { Describe("flow done"); return Done(); }
    return Stay();
}
