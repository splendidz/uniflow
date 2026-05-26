// load_picker.h - carries a raw part A -> B. One flow per round trip.
// Motion pattern: "Cmd" step issues SetTarget and Advances; "WaitAt"
// step polls InPosition with Stay(). Gripper is axis-driven the same way.
#pragma once

#include "globals.h"
#include "picker_motion.h"

class LoadPicker : public uniflow::Uniflow<LoadPicker>,
                   public PickerMotion
{
    UF_UNIFLOW_IMPLEMENT(LoadPicker);

public:
    explicit LoadPicker(uniflow::Runtime& rt)
        : uniflow::Uniflow<LoadPicker>(rt),
          PickerMotion(GlobalGeometry::kZoneA_mm) {}

    StepResult OnLoad_Begin();

private:
    StepResult OnLoad_CmdMoveToSource();
    StepResult OnLoad_WaitAtSource();
    StepResult OnLoad_CmdLowerToPick();
    StepResult OnLoad_WaitAtPickDown();
    StepResult OnLoad_HandGrip();
    StepResult OnLoad_CmdLiftWithPart();
    StepResult OnLoad_WaitAtPickUp();
    StepResult OnLoad_CmdMoveToDest();
    StepResult OnLoad_WaitAtDest();
    StepResult OnLoad_CmdLowerToPlace();
    StepResult OnLoad_WaitAtPlaceDown();
    StepResult OnLoad_HandRelease();
    StepResult OnLoad_CmdLiftEmpty();
    StepResult OnLoad_WaitAtPlaceUp();
    StepResult OnLoad_CmdRetreat();
    StepResult OnLoad_WaitAtRetreat();

    // Defined out-of-line in unload_picker.cpp once UnloadPicker is
    // visible. Forward declaration only.
    bool PartnerInZoneB() const;
};
