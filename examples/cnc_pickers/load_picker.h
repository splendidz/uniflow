// ======================================================================
//  load_picker.h - carries a raw part A -> B.
//
//  One flow == one A->B->A round trip. The Orchestrator launches this
//  picker when zone A has a part and the picker is Idle. The picker
//  itself decides to park at the B-safety-gap boundary if Stage is not
//  yet ready to receive (gives the "prefetch + wait" behaviour).
//
//  Step pattern: each motion is split into a "command" step (issues
//  SetTarget, advances immediately) and a "wait at" step (polls
//  InPosition with Stay() for fastest possible response). Gripper grip
//  / release is also axis-driven: SetTarget on finger_axis_, Stay()
//  while the fingers traverse, advance on InPosition.
//
//  Step bodies are defined in load_picker.cpp.
// ======================================================================
#pragma once

#include "globals.h"
#include "picker_motion.h"

class LoadPicker : public uniflow::Uniflow<LoadPicker>,
                   public PickerMotion
{
    UF_USES_UNIFLOW(LoadPicker);

public:
    LoadPicker();

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
