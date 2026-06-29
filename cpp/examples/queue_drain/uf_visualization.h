// uf_visualization.h - the renderer side. Two halves:
//   - Flow_Visualization: a module on the pump thread whose one perpetual step
//     copies live sender / receiver / mailbox state into g_snap each tick.
//   - RunVisualisation(): the main-thread entry. A background thread redraws the
//     ANSI dashboard ~20 fps; the main thread blocks on stdin so one Enter quits.
#pragma once

#include "globals.h"

class Flow_Visualization : public uniflow::Uniflow<Flow_Visualization>
{
public:
    explicit Flow_Visualization(uniflow::Runtime& rt);

    // The single perpetual snapshot task (public so app.Start() launches it with
    // task.StartFlow()). Its one step copies state into g_snap every round.
    struct Task_Snapshot : uniflow::Task<Flow_Visualization>
    {
        StepResult Entry() override { return Step1_Tick(); }

    private:
        StepResult Step1_Tick();

        // Wall-clock timing of processed jobs, used to publish the cycle speed.
        std::chrono::steady_clock::time_point start_{};
        std::chrono::steady_clock::time_point last_job_{};
        int    last_processed_ = 0;
        double last_cycle_ms_  = 0.0;
        bool   started_        = false;
    } task_snapshot_;
};

// Main-thread render loop: background draw thread + stdin getline (Enter quits).
void RunVisualisation();
