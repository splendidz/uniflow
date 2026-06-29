// uf_orchestrator.h - line-level scheduler. Owns when raw parts spawn and when
// each module's next flow starts; pickers / Flow_Stage never decide. Polled
// every pump round (Config::stay_sleep cadence, default 20ms). One perpetual
// task (Schedule) whose single step polls the line forever.
#pragma once

#include "globals.h"

class Flow_Orchestrator : public uniflow::Uniflow<Flow_Orchestrator>
{
public:
    explicit Flow_Orchestrator(uniflow::Runtime& rt);

    // The single perpetual Schedule task (public so app.Start() launches it with
    // task.StartFlow()). Its one step polls the line and drives every module.
    struct Task_Schedule : uniflow::Task<Flow_Orchestrator>
    {
        StepResult Entry() override { return Step1_Tick(); }

    private:
        StepResult Step1_Tick();
    } task_schedule_;

private:
    // Each picks the next task for a module and launches it when the module is
    // idle (task.StartFlow()). The pickers/Stage never sequence themselves.
    void TryCreateRawPart();
    void TryDriveLoadPicker();
    void TryDriveStage();
    void TryDriveUnloadPicker();
};
