// orchestrator.h - line-level scheduler. Owns when raw parts spawn and
// when each module's next flow starts; pickers/Stage never decide.
// Hybrid poll+event: kSchedTick backstop for time-driven decisions
// (raw-part spawn), Runtime::Notify() for state-driven ones.
#pragma once

#include "globals.h"

#include <random>

class Orchestrator : public uniflow::Uniflow<Orchestrator>
{
    UF_UNIFLOW_IMPLEMENT(Orchestrator);

public:
    explicit Orchestrator(uniflow::Runtime& rt)
        : uniflow::Uniflow<Orchestrator>(rt) {}

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
