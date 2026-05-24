// ======================================================================
//  unload_picker.h - carries the finished part B -> C.
//
//  Same shape as LoadPicker but the SOURCE is the contested B zone, so
//  the entry-to-B check happens on the way in (not just on the dest dip).
//
//  Step bodies are defined in unload_picker.cpp. The cross-picker queries
//  LoadPicker::PartnerInZoneB / UnloadPicker::PartnerInZoneB are also
//  defined there: by including load_picker.h first, both classes are
//  visible by the time the out-of-line definitions need them.
// ======================================================================
#pragma once

#include "globals.h"
#include "picker_motion.h"

class UnloadPicker : public uniflow::Uniflow<UnloadPicker>,
                     public PickerMotion
{
    UF_USES_UNIFLOW(UnloadPicker);

public:
    UnloadPicker();

    StepResult OnUnload_Begin();

private:
    StepResult OnUnload_CmdMoveToSource();
    StepResult OnUnload_WaitAtSource();
    StepResult OnUnload_CmdLowerToPick();
    StepResult OnUnload_WaitAtPickDown();
    StepResult OnUnload_HandGrip();
    StepResult OnUnload_CmdLiftWithPart();
    StepResult OnUnload_WaitAtPickUp();
    StepResult OnUnload_CmdMoveToUnload();
    StepResult OnUnload_WaitAtUnload();
    StepResult OnUnload_CmdLowerToPlace();
    StepResult OnUnload_WaitAtPlaceDown();
    StepResult OnUnload_HandRelease();
    StepResult OnUnload_CmdLiftEmpty();
    StepResult OnUnload_WaitAtPlaceUp();
    StepResult OnUnload_CmdRetreat();
    StepResult OnUnload_WaitAtRetreat();

    bool PartnerInZoneB() const;
};
