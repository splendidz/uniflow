// ======================================================================
//  orchestrator.h - the line-level scheduler.
//
//  Owns ALL scheduling decisions:
//    - Creating raw parts at zone A on a random cadence (the old Loader
//      module, absorbed - "a part appears" is a single decision, no
//      flow needed).
//    - Kicking off LoadPicker / UnloadPicker / Stage flows when their
//      precondition is satisfied.
//
//  Pickers and Stage never decide what to do next.
//
//  Scheduling model: hybrid poll + event.
//    - The OnSchedule_Tick step parks on Wait(kSchedTick). The poll is
//      the backstop for the ONE time-driven decision: creating a raw
//      part when its randomised gap elapses.
//    - The state-driven Try* decisions react to events instead of the
//      poll. State mutators (GlobalEnv::CreateFakeZoneAPart,
//      Stage::OnRawPartReceived / OnProcessedPartTaken, Stage's
//      ProcessedPartReady transition) call uniflow::NotifyAll(), which
//      flips force_wake and bypasses this module's wait gate so the
//      next Try* runs on the same pump round - not kSchedTick later.
//      Flow termination (e.g. picker FLOW END) already auto-Notifies
//      via uniflow's own ClearFlow path.
//
//  Step bodies are defined in orchestrator.cpp.
// ======================================================================
#pragma once

#include "globals.h"

#include <random>

class Orchestrator : public uniflow::Uniflow<Orchestrator>
{
    UF_SINGLETON(Orchestrator);

public:
    StepResult OnSchedule_Begin();

private:
    StepResult OnSchedule_Tick();

    void ScheduleNextLoaderCreate();
    void TryCreateFakeRawPart();
    void TryStartLoadPicker();
    void TryStartStageProcessing();
    void TryStartUnloadPicker();

    std::mt19937       rng_{std::random_device{}()};
    uniflow::TimePoint next_spawn_at_;
};
