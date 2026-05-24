// ======================================================================
//  orchestrator.cpp - scheduling step + Try* triggers.
//
//  Try* are called every kSchedTick (backstop) AND every time a state
//  mutator elsewhere calls uniflow::NotifyAll() (event path). The body
//  itself stays oblivious to which path woke it; it just re-evaluates
//  every precondition and acts on whatever now passes.
// ======================================================================
#include "orchestrator.h"

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

    // Time-driven (only this one needs the poll cadence to fire):
    TryCreateFakeRawPart();

    // Event-driven (woken by NotifyAll() in the relevant mutators;
    // re-evaluated here on every tick path too, so the poll backstop
    // covers any missed wake):
    TryStartLoadPicker();
    TryStartStageProcessing();
    TryStartUnloadPicker();

    return Wait(GlobalTiming::kSchedTick);
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
    // CreateFakeZoneAPart() itself calls NotifyAll() so TryStartLoadPicker
    // on the next tick (which is "right now" - we run it below) sees the
    // new part without waiting kSchedTick.
    GlobalEnv::CreateFakeZoneAPart();
    ScheduleNextLoaderCreate();
}

void Orchestrator::TryStartLoadPicker()
{
    auto& picker = LoadPicker::GetInst();
    if (!picker.IsIdle())              return;
    if (!GlobalEnv::ZoneAHasPart())    return;
    // Launch even if Stage is not yet Idle - the picker will park at
    // the B-zone boundary. This is the "prefetch" behaviour.
    UF_START_FLOW(picker, OnLoad_Begin);
}

void Orchestrator::TryStartUnloadPicker()
{
    auto& picker = UnloadPicker::GetInst();
    if (!picker.IsIdle()) return;

    // Prefetch as early as a part is on its way to Stage so the picker
    // creeps to the B-safety gap in parallel with the LoadPicker's
    // approach, instead of standing idle at C until Stage transitions:
    //   - Stage already has a part loaded / processing / ready
    //   - OR LoadPicker is currently carrying one toward B
    auto& loader = LoadPicker::GetInst();
    auto  st     = Stage::inst().state();
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
    auto& stage = Stage::inst();
    if (!stage.IsIdle())                              return;
    if (stage.state() != StageState::RawPartLoaded)   return;
    UF_START_FLOW(stage, OnProcess_Begin);
}
