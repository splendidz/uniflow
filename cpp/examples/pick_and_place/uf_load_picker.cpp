#include "uf_load_picker.h"

#include "app.h"
#include "uf_stage.h"

using namespace uniflow;

// -- Construction: borrow the three axes from the factory, bind the tasks. --

Flow_LoadPicker::Flow_LoadPicker(uniflow::Runtime& rt)
    : uniflow::Uniflow<Flow_LoadPicker>(rt, "Flow_LoadPicker"),
      x_(MotorIOFactory::inst().CreateAxis(
          "load_x", GlobalGeometry::kZoneA_mm, GlobalGeometry::kXSpeed_mm_per_s)),
      z_(MotorIOFactory::inst().CreateAxis(
          "load_z", GlobalGeometry::kZUp_mm, GlobalGeometry::kZSpeed_mm_per_s)),
      finger_(MotorIOFactory::inst().CreateAxis(
          "load_finger", GlobalGeometry::kFingerOpen_mm,
          GlobalGeometry::kFingerSpeed_mm_per_s))
{
    AddTask(task_pick_);
    AddTask(task_place_);
}

// -- Motion state, read from the borrowed axes. --

double Flow_LoadPicker::X_mm() const
{
    return x_->Position();
}

double Flow_LoadPicker::Z_mm() const
{
    return z_->Position();
}

double Flow_LoadPicker::FingerGap_mm() const
{
    return finger_->Position();
}

bool Flow_LoadPicker::Carrying() const
{
    return carrying_;
}

bool Flow_LoadPicker::InsideZoneB() const
{
    return GlobalGeometry::InsideZoneB(X_mm());
}

// ======================================================================
//  Task: Pick (zone A) - steps reach the borrowed axes through flow().
// ======================================================================

StepResult Flow_LoadPicker::Task_Pick::Step1_CmdMoveToSource()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    Describe("cmd: move to zone A");
    flow().x_->Move(GlobalGeometry::kZoneA_mm);
    return Next(UF_FN(Step2_WaitAtSource));
}

StepResult Flow_LoadPicker::Task_Pick::Step2_WaitAtSource()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    Describe("approaching A");
    if (flow().x_->InPosition())
    {
        return Next(UF_FN(Step3_CmdLowerToPick));
    }
    return Stay();
}

StepResult Flow_LoadPicker::Task_Pick::Step3_CmdLowerToPick()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    Describe("cmd: lower to pick");
    flow().z_->Move(GlobalGeometry::kZDown_mm);
    return Next(UF_FN(Step4_WaitAtPickDown));
}

StepResult Flow_LoadPicker::Task_Pick::Step4_WaitAtPickDown()
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

StepResult Flow_LoadPicker::Task_Pick::Step5_HandGrip()
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
    GlobalEnv::ConsumeZoneAPart();
    flow().carrying_ = true;
    return Next(UF_FN(Step6_CmdLiftWithPart));
}

StepResult Flow_LoadPicker::Task_Pick::Step6_CmdLiftWithPart()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    Describe("cmd: lift with part");
    flow().z_->Move(GlobalGeometry::kZUp_mm);
    return Next(UF_FN(Step7_WaitAtPickUp));
}

StepResult Flow_LoadPicker::Task_Pick::Step7_WaitAtPickUp()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    Describe("lifting with part");
    // Pick task done: part is up and carried. The flow goes idle; the
    // orchestrator launches the Place task next (it sees Carrying()).
    if (flow().z_->InPosition())
    {
        return Done();
    }
    return Stay();
}

// ======================================================================
//  Task: Place (zone B) - gated by Flow_Stage readiness + partner.
// ======================================================================

StepResult Flow_LoadPicker::Task_Place::Step1_CmdMoveToDest()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    bool may_enter_B =
        App::inst().stage.ReadyToReceiveRawPart() && !flow().PartnerInZoneB();
    if (!flow().InsideZoneB() && !may_enter_B)
    {
        flow().x_->Move(GlobalGeometry::kZoneB_mm - GlobalGeometry::kBSafetyGap_mm);
        if (!flow().x_->InPosition())
        {
            Describe("moving to A-gap");
            return Stay();
        }
        Describe("parked at A-gap: stage=", ToString(App::inst().stage.state()),
                 " partner_in_B=", flow().PartnerInZoneB());
        return Stay();
    }
    Describe("cmd: move to zone B");
    flow().x_->Move(GlobalGeometry::kZoneB_mm);
    return Next(UF_FN(Step2_WaitAtDest));
}

StepResult Flow_LoadPicker::Task_Place::Step2_WaitAtDest()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    Describe("approaching B");
    if (flow().x_->InPosition())
    {
        return Next(UF_FN(Step3_CmdLowerToPlace));
    }
    return Stay();
}

StepResult Flow_LoadPicker::Task_Place::Step3_CmdLowerToPlace()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    Describe("cmd: lower to place");
    flow().z_->Move(GlobalGeometry::kZDown_mm);
    return Next(UF_FN(Step4_WaitAtPlaceDown));
}

StepResult Flow_LoadPicker::Task_Place::Step4_WaitAtPlaceDown()
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

StepResult Flow_LoadPicker::Task_Place::Step5_HandRelease()
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
    App::inst().stage.OnRawPartReceived();
    flow().carrying_ = false;
    return Next(UF_FN(Step6_CmdLiftEmpty));
}

StepResult Flow_LoadPicker::Task_Place::Step6_CmdLiftEmpty()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    Describe("cmd: lift empty");
    flow().z_->Move(GlobalGeometry::kZUp_mm);
    return Next(UF_FN(Step7_WaitAtPlaceUp));
}

StepResult Flow_LoadPicker::Task_Place::Step7_WaitAtPlaceUp()
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

StepResult Flow_LoadPicker::Task_Place::Step8_CmdRetreat()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    Describe("cmd: retreat to A");
    flow().x_->Move(GlobalGeometry::kZoneA_mm);
    return Next(UF_FN(Step9_WaitAtRetreat));
}

StepResult Flow_LoadPicker::Task_Place::Step9_WaitAtRetreat()
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
