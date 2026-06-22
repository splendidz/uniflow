// uf_unload_picker.cpp - flow steps + PartnerInZoneB for both pickers.
#include "uf_unload_picker.h"

#include "app.h"
#include "uf_load_picker.h"
#include "uf_stage.h"

using namespace uniflow;

// -- Construction: borrow the three axes from the factory, bind the tasks. --

Flow_UnloadPicker::Flow_UnloadPicker(uniflow::Runtime& rt)
    : uniflow::Uniflow<Flow_UnloadPicker>(rt, "Flow_UnloadPicker"),
      x_(MotorIOFactory::inst().CreateAxis(
          "unload_x", GlobalGeometry::kZoneC_mm, GlobalGeometry::kXSpeed_mm_per_s)),
      z_(MotorIOFactory::inst().CreateAxis(
          "unload_z", GlobalGeometry::kZUp_mm, GlobalGeometry::kZSpeed_mm_per_s)),
      finger_(MotorIOFactory::inst().CreateAxis(
          "unload_finger", GlobalGeometry::kFingerOpen_mm,
          GlobalGeometry::kFingerSpeed_mm_per_s))
{
    AddTask(ctx_pick_);
    AddTask(ctx_place_);
}

// -- Motion state, read from the borrowed axes. --

double Flow_UnloadPicker::X_mm() const
{
    return x_->Position();
}

double Flow_UnloadPicker::Z_mm() const
{
    return z_->Position();
}

double Flow_UnloadPicker::FingerGap_mm() const
{
    return finger_->Position();
}

bool Flow_UnloadPicker::Carrying() const
{
    return carrying_;
}

bool Flow_UnloadPicker::InsideZoneB() const
{
    return GlobalGeometry::InsideZoneB(X_mm());
}

// ======================================================================
//  Task: Pick (zone B, contested).
// ======================================================================

StepResult Flow_UnloadPicker::Task_Pick::Step1_CmdMoveToSource()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }

    auto& load = App::inst().load;
    auto  st   = App::inst().stage.state();
    bool  stage_past_loading =
        st == StageState::Prepared
     || st == StageState::Machined
     || st == StageState::ProcessedPartReady;
    // load.Carrying() catches "approaching B with a part"; InsideZoneB()
    // catches "still lifting/retreating in B after the handoff".
    bool  load_threatens_B = load.Carrying() || load.InsideZoneB();
    bool  may_enter_B      = stage_past_loading && !load_threatens_B;

    if (!flow().InsideZoneB() && !may_enter_B)
    {
        flow().x_->Move(GlobalGeometry::kZoneB_mm + GlobalGeometry::kBSafetyGap_mm);
        if (!flow().x_->InPosition())
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
    flow().x_->Move(GlobalGeometry::kZoneB_mm);
    return Next(UF_FN(Step2_WaitAtSource));
}

StepResult Flow_UnloadPicker::Task_Pick::Step2_WaitAtSource()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    Describe("approaching B");
    if (flow().x_->InPosition())
    {
        return Next(UF_FN(Step3_CmdLowerToPick));
    }
    return Stay();
}

StepResult Flow_UnloadPicker::Task_Pick::Step3_CmdLowerToPick()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    if (!App::inst().stage.ReadyToHandOffProcessedPart())
    {
        Describe("hovering above B: stage=",
                 ToString(App::inst().stage.state()));
        return Stay();
    }
    Describe("cmd: lower to pick");
    flow().z_->Move(GlobalGeometry::kZDown_mm);
    return Next(UF_FN(Step4_WaitAtPickDown));
}

StepResult Flow_UnloadPicker::Task_Pick::Step4_WaitAtPickDown()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    Describe("lowering to pick");
    if (flow().z_->InPosition())
    {
        return Next(UF_FN(Step5_HandGrip));
    }
    return Stay();
}

StepResult Flow_UnloadPicker::Task_Pick::Step5_HandGrip()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    Describe("closing gripper");
    flow().finger_->Move(0.0);
    if (!flow().finger_->InPosition())
    {
        return Stay();
    }
    App::inst().stage.OnProcessedPartTaken();
    flow().carrying_ = true;
    return Next(UF_FN(Step6_CmdLiftWithPart));
}

StepResult Flow_UnloadPicker::Task_Pick::Step6_CmdLiftWithPart()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    Describe("cmd: lift with part");
    flow().z_->Move(GlobalGeometry::kZUp_mm);
    return Next(UF_FN(Step7_WaitAtPickUp));
}

StepResult Flow_UnloadPicker::Task_Pick::Step7_WaitAtPickUp()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    Describe("lifting with part");
    // Pick task done: part is up and carried. The orchestrator launches the
    // Place task next (it sees Carrying()).
    if (flow().z_->InPosition())
    {
        return Done();
    }
    return Stay();
}

// ======================================================================
//  Task: Place (zone C).
// ======================================================================

StepResult Flow_UnloadPicker::Task_Place::Step1_CmdMoveToUnload()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    Describe("cmd: move to zone C");
    flow().x_->Move(GlobalGeometry::kZoneC_mm);
    return Next(UF_FN(Step2_WaitAtUnload));
}

StepResult Flow_UnloadPicker::Task_Place::Step2_WaitAtUnload()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    Describe("approaching C");
    if (flow().x_->InPosition())
    {
        return Next(UF_FN(Step3_CmdLowerToPlace));
    }
    return Stay();
}

StepResult Flow_UnloadPicker::Task_Place::Step3_CmdLowerToPlace()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    Describe("cmd: lower to place");
    flow().z_->Move(GlobalGeometry::kZDown_mm);
    return Next(UF_FN(Step4_WaitAtPlaceDown));
}

StepResult Flow_UnloadPicker::Task_Place::Step4_WaitAtPlaceDown()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    Describe("lowering to place");
    if (flow().z_->InPosition())
    {
        return Next(UF_FN(Step5_HandRelease));
    }
    return Stay();
}

StepResult Flow_UnloadPicker::Task_Place::Step5_HandRelease()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    Describe("opening gripper");
    flow().finger_->Move(GlobalGeometry::kFingerOpen_mm);
    if (!flow().finger_->InPosition())
    {
        return Stay();
    }
    GlobalEnv::IncDelivered();
    flow().carrying_ = false;
    return Next(UF_FN(Step6_CmdLiftEmpty));
}

StepResult Flow_UnloadPicker::Task_Place::Step6_CmdLiftEmpty()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    Describe("cmd: lift empty");
    flow().z_->Move(GlobalGeometry::kZUp_mm);
    return Next(UF_FN(Step7_WaitAtPlaceUp));
}

StepResult Flow_UnloadPicker::Task_Place::Step7_WaitAtPlaceUp()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    Describe("lifting empty");
    if (flow().z_->InPosition())
    {
        return Next(UF_FN(Step8_CmdRetreat));
    }
    return Stay();
}

StepResult Flow_UnloadPicker::Task_Place::Step8_CmdRetreat()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    Describe("cmd: retreat to C");
    flow().x_->Move(GlobalGeometry::kZoneC_mm);
    return Next(UF_FN(Step9_WaitAtRetreat));
}

StepResult Flow_UnloadPicker::Task_Place::Step9_WaitAtRetreat()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    Describe("retreating");
    if (flow().x_->InPosition())
    {
        Describe("flow done");
        return Done();
    }
    return Stay();
}

// ======================================================================
//  Partner-in-B query (both pickers visible here).
// ======================================================================

bool Flow_LoadPicker::PartnerInZoneB() const
{
    return App::inst().unload.InsideZoneB();
}

bool Flow_UnloadPicker::PartnerInZoneB() const
{
    return App::inst().load.InsideZoneB();
}
