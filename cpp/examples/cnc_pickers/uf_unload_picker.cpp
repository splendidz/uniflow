// unload_picker.cpp - flow steps + PartnerInZoneB for both pickers.
#include "uf_unload_picker.h"

#include "app.h"
#include "uf_load_picker.h"
#include "uf_stage.h"

UF_UnloadPicker::StepResult UF_UnloadPicker::OnUnload_Begin()
{
    Describe("flow start");
    return UF_NEXT(OnUnload_CmdMoveToSource);
}

// --- Source (zone B). Enter B only when UF_Stage has already started
//     processing AND UF_LoadPicker is neither in B nor carrying toward it.
//     Then hover at B with Z up; Z lowering waits for hand-off readiness. ---

UF_UnloadPicker::StepResult UF_UnloadPicker::OnUnload_CmdMoveToSource()
{
    if (GlobalEnv::Stop()) return Done();

    auto& load = App::inst().load;
    auto  st   = App::inst().stage.state();
    bool  stage_past_loading =
        st == StageState::Processing
     || st == StageState::ProcessedPartReady;
    // load.Carrying() catches "approaching B with a part"; InsideZoneB()
    // catches "still lifting/retreating in B after the handoff".
    bool  load_threatens_B = load.Carrying() || load.InsideZoneB();
    bool  may_enter_B      = stage_past_loading && !load_threatens_B;

    if (!InsideZoneB() && !may_enter_B)
    {
        x_axis_.SetTarget(GlobalGeometry::kZoneB_mm
                          + GlobalGeometry::kBSafetyGap_mm);
        x_axis_.Update(GlobalGeometry::kXSpeed_mm_per_s);
        if (!x_axis_.InPosition())
        {
            Describe("moving to C-gap");
            return Stay();
        }
        Describe("parked at C-gap: stage=", ToString(st),
                 " load_carry=", load.Carrying(),
                 " load_in_B=", load.InsideZoneB());
        return Stay();
    }
    Describe("cmd: move to zone B");
    x_axis_.SetTarget(GlobalGeometry::kZoneB_mm);
    return UF_NEXT(OnUnload_WaitAtSource);
}

UF_UnloadPicker::StepResult UF_UnloadPicker::OnUnload_WaitAtSource()
{
    if (GlobalEnv::Stop()) return Done();
    x_axis_.Update(GlobalGeometry::kXSpeed_mm_per_s);
    Describe("approaching B");
    if (x_axis_.InPosition()) return UF_NEXT(OnUnload_CmdLowerToPick);
    return Stay();
}

UF_UnloadPicker::StepResult UF_UnloadPicker::OnUnload_CmdLowerToPick()
{
    if (GlobalEnv::Stop()) return Done();
    if (!App::inst().stage.ReadyToHandOffProcessedPart())
    {
        Describe("hovering above B: stage=",
                 ToString(App::inst().stage.state()));
        return Stay();
    }
    Describe("cmd: lower to pick");
    z_axis_.SetTarget(GlobalGeometry::kZDown_mm);
    return UF_NEXT(OnUnload_WaitAtPickDown);
}

UF_UnloadPicker::StepResult UF_UnloadPicker::OnUnload_WaitAtPickDown()
{
    if (GlobalEnv::Stop()) return Done();
    z_axis_.Update(GlobalGeometry::kZSpeed_mm_per_s);
    Describe("lowering to pick");
    if (z_axis_.InPosition()) return UF_NEXT(OnUnload_HandGrip);
    return Stay();
}

UF_UnloadPicker::StepResult UF_UnloadPicker::OnUnload_HandGrip()
{
    if (GlobalEnv::Stop()) return Done();
    finger_axis_.SetTarget(0.0);
    finger_axis_.Update(GlobalGeometry::kFingerSpeed_mm_per_s);
    Describe("closing gripper");
    if (!finger_axis_.InPosition()) return Stay();
    App::inst().stage.OnProcessedPartTaken();
    SetCarrying(true);
    return UF_NEXT(OnUnload_CmdLiftWithPart);
}

UF_UnloadPicker::StepResult UF_UnloadPicker::OnUnload_CmdLiftWithPart()
{
    if (GlobalEnv::Stop()) return Done();
    Describe("cmd: lift with part");
    z_axis_.SetTarget(GlobalGeometry::kZUp_mm);
    return UF_NEXT(OnUnload_WaitAtPickUp);
}

UF_UnloadPicker::StepResult UF_UnloadPicker::OnUnload_WaitAtPickUp()
{
    if (GlobalEnv::Stop()) return Done();
    z_axis_.Update(GlobalGeometry::kZSpeed_mm_per_s);
    Describe("lifting with part");
    if (z_axis_.InPosition()) return UF_NEXT(OnUnload_CmdMoveToUnload);
    return Stay();
}

// --- Destination (zone C) ---

UF_UnloadPicker::StepResult UF_UnloadPicker::OnUnload_CmdMoveToUnload()
{
    if (GlobalEnv::Stop()) return Done();
    Describe("cmd: move to zone C");
    x_axis_.SetTarget(GlobalGeometry::kZoneC_mm);
    return UF_NEXT(OnUnload_WaitAtUnload);
}

UF_UnloadPicker::StepResult UF_UnloadPicker::OnUnload_WaitAtUnload()
{
    if (GlobalEnv::Stop()) return Done();
    x_axis_.Update(GlobalGeometry::kXSpeed_mm_per_s);
    Describe("approaching C");
    if (x_axis_.InPosition()) return UF_NEXT(OnUnload_CmdLowerToPlace);
    return Stay();
}

UF_UnloadPicker::StepResult UF_UnloadPicker::OnUnload_CmdLowerToPlace()
{
    if (GlobalEnv::Stop()) return Done();
    Describe("cmd: lower to place");
    z_axis_.SetTarget(GlobalGeometry::kZDown_mm);
    return UF_NEXT(OnUnload_WaitAtPlaceDown);
}

UF_UnloadPicker::StepResult UF_UnloadPicker::OnUnload_WaitAtPlaceDown()
{
    if (GlobalEnv::Stop()) return Done();
    z_axis_.Update(GlobalGeometry::kZSpeed_mm_per_s);
    Describe("lowering to place");
    if (z_axis_.InPosition()) return UF_NEXT(OnUnload_HandRelease);
    return Stay();
}

UF_UnloadPicker::StepResult UF_UnloadPicker::OnUnload_HandRelease()
{
    if (GlobalEnv::Stop()) return Done();
    finger_axis_.SetTarget(GlobalGeometry::kFingerOpen_mm);
    finger_axis_.Update(GlobalGeometry::kFingerSpeed_mm_per_s);
    Describe("opening gripper");
    if (!finger_axis_.InPosition()) return Stay();
    GlobalEnv::IncDelivered();
    SetCarrying(false);
    return UF_NEXT(OnUnload_CmdLiftEmpty);
}

UF_UnloadPicker::StepResult UF_UnloadPicker::OnUnload_CmdLiftEmpty()
{
    if (GlobalEnv::Stop()) return Done();
    Describe("cmd: lift empty");
    z_axis_.SetTarget(GlobalGeometry::kZUp_mm);
    return UF_NEXT(OnUnload_WaitAtPlaceUp);
}

UF_UnloadPicker::StepResult UF_UnloadPicker::OnUnload_WaitAtPlaceUp()
{
    if (GlobalEnv::Stop()) return Done();
    z_axis_.Update(GlobalGeometry::kZSpeed_mm_per_s);
    Describe("lifting empty");
    if (z_axis_.InPosition()) return UF_NEXT(OnUnload_CmdRetreat);
    return Stay();
}

UF_UnloadPicker::StepResult UF_UnloadPicker::OnUnload_CmdRetreat()
{
    if (GlobalEnv::Stop()) return Done();
    Describe("cmd: retreat to C");
    x_axis_.SetTarget(GlobalGeometry::kZoneC_mm);
    return UF_NEXT(OnUnload_WaitAtRetreat);
}

UF_UnloadPicker::StepResult UF_UnloadPicker::OnUnload_WaitAtRetreat()
{
    if (GlobalEnv::Stop()) return Done();
    x_axis_.Update(GlobalGeometry::kXSpeed_mm_per_s);
    Describe("retreating");
    if (x_axis_.InPosition()) { Describe("flow done"); return Done(); }
    return Stay();
}

bool UF_LoadPicker::PartnerInZoneB() const
{
    return App::inst().unload.InsideZoneB();
}
bool UF_UnloadPicker::PartnerInZoneB() const
{
    return App::inst().load.InsideZoneB();
}
