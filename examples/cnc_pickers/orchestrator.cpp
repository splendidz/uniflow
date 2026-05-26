#include "orchestrator.h"

#include "app.h"
#include "load_picker.h"
#include "stage.h"
#include "unload_picker.h"

Orchestrator::StepResult Orchestrator::OnSchedule_Begin()
{
    ScheduleNextLoaderCreate();
    return UF_NEXT(OnSchedule_Tick);
}

Orchestrator::StepResult Orchestrator::OnSchedule_Tick()
{
    if (GlobalEnv::Stop())
        return Done();

    TryCreateFakeRawPart();        // time-driven
    TryStartLoadPicker();          // event-driven (poll = backstop)
    TryStartStageProcessing();
    TryStartUnloadPicker();

    return Stay(GlobalTiming::kSchedTick);
}

void Orchestrator::ScheduleNextLoaderCreate()
{
    auto min_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      GlobalTiming::kLoaderMinGap).count();
    auto max_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      GlobalTiming::kLoaderMaxGap).count();
    std::uniform_int_distribution<long long> d(min_ms, max_ms);
    next_spawn_at_ = uniflow::Clock::now()
                     + std::chrono::milliseconds(d(rng_));
}

void Orchestrator::TryCreateFakeRawPart()
{
    if (GlobalEnv::ZoneAHasPart())                 return;
    if (uniflow::Clock::now() < next_spawn_at_)    return;
    GlobalEnv::CreateFakeZoneAPart();
    ScheduleNextLoaderCreate();
}

void Orchestrator::TryStartLoadPicker()
{
    auto& picker = App::inst().load;
    if (!picker.IsIdle())              return;
    if (!GlobalEnv::ZoneAHasPart())    return;
    // Prefetch: launch even while Stage isn't Idle - picker parks at the
    // A-side gap.
    UF_START_FLOW(picker, OnLoad_Begin);
}

void Orchestrator::TryStartUnloadPicker()
{
    auto& picker = App::inst().unload;
    if (!picker.IsIdle()) return;

    // Prefetch as soon as a part is incoming so the unload picker is
    // already hovering above B when Stage hands off.
    auto& loader = App::inst().load;
    auto  st     = App::inst().stage.state();
    bool  stage_has_part_incoming =
        st == StageState::RawPartLoaded
     || st == StageState::Processing
     || st == StageState::ProcessedPartReady;
    bool  loader_bringing_part = !loader.IsIdle() && loader.Carrying();
    if (!stage_has_part_incoming && !loader_bringing_part)
        return;
    UF_START_FLOW(picker, OnUnload_Begin);
}

void Orchestrator::TryStartStageProcessing()
{
    auto& stage = App::inst().stage;
    if (!stage.IsIdle())                              return;
    if (stage.state() != StageState::RawPartLoaded)   return;
    UF_START_FLOW(stage, OnProcess_Begin);
}
