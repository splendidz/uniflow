#include "uf_orchestrator.h"

#include "app.h"
#include "uf_load_picker.h"
#include "uf_stage.h"
#include "uf_unload_picker.h"

using namespace uniflow;

Flow_Orchestrator::Flow_Orchestrator(uniflow::Runtime& rt)
    : uniflow::Uniflow<Flow_Orchestrator>(rt, "Flow_Orchestrator")
{
    AddTask(task_schedule_);
}

StepResult Flow_Orchestrator::Task_Schedule::Step1_Tick()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    flow().TryCreateRawPart();
    flow().TryDriveLoadPicker();
    flow().TryDriveStage();
    flow().TryDriveUnloadPicker();

    return Stay();
}

void Flow_Orchestrator::TryCreateRawPart()
{
    // A fresh raw part is staged the instant zone A is empty - the feeder is
    // never the bottleneck in this demo.
    if (GlobalEnv::ZoneAHasPart())
    {
        return;
    }
    GlobalEnv::CreateFakeZoneAPart();
}

void Flow_Orchestrator::TryDriveLoadPicker()
{
    auto& picker = App::inst().load;
    if (!picker.IsIdle())
    {
        return;
    }
    // Carrying a part -> deliver it (Place); otherwise grab the next one (Pick).
    // The Place task itself parks at the A-gap until zone B is clear.
    if (picker.Carrying())
    {
        picker.task_place_.StartFlow();
    }
    else if (GlobalEnv::ZoneAHasPart())
    {
        picker.task_pick_.StartFlow();
    }
}

void Flow_Orchestrator::TryDriveStage()
{
    auto& stage = App::inst().stage;
    if (!stage.IsIdle())
    {
        return;
    }
    // One task per machining phase, launched as the previous phase completes.
    switch (stage.state())
    {
    case StageState::RawPartLoaded:
        stage.task_prepare_.StartFlow();
        break;
    case StageState::Prepared:
        stage.task_process_.StartFlow();
        break;
    case StageState::Machined:
        stage.task_cleanup_.StartFlow();
        break;
    default:
        break;
    }
}

void Flow_Orchestrator::TryDriveUnloadPicker()
{
    auto& picker = App::inst().unload;
    if (!picker.IsIdle())
    {
        return;
    }
    if (picker.Carrying())
    {
        picker.task_place_.StartFlow();
        return;
    }
    // Prefetch the Pick task as soon as a part is incoming, so the picker is
    // already hovering above B when the Stage finishes; the Pick task waits
    // for hand-off readiness internally.
    auto st = App::inst().stage.state();
    bool stage_has_part_incoming =
        st == StageState::Prepared
     || st == StageState::Machined
     || st == StageState::ProcessedPartReady;
    if (stage_has_part_incoming)
    {
        picker.task_pick_.StartFlow();
    }
}
