// orchestrator.h - line-level scheduler. Owns when raw parts spawn and
// when each module's next flow starts; pickers/UF_Stage never decide.
// Polled every pump round (Config::stay_sleep cadence, default 20ms) for
// both time-driven (raw-part spawn) and state-driven decisions.
#pragma once

#include "globals.h"

#include <random>

class UF_Orchestrator : public uniflow::Uniflow<UF_Orchestrator>
{
    UF_UNIFLOW_IMPLEMENT(UF_Orchestrator);

public:
    explicit UF_Orchestrator(uniflow::Runtime& rt)
        : uniflow::Uniflow<UF_Orchestrator>(rt) {}

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
