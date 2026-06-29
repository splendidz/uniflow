// uf_visualization.h - one task that copies prof / friend / mailbox / student
// state into g_snap every pump round (FEATURE FOCUS: a renderer feed is just
// another flow on the same pump - it reads every peer with plain member access,
// no lock, because they share the thread). The actual drawing runs on a
// background render thread (RunConsoleRenderer) that reads g_snap and paints an
// ANSI dashboard, leaving the main thread free to block on std::getline.
#pragma once

#include "uniflow.hpp"

#include <atomic>

class Flow_Visualization : public uniflow::Uniflow<Flow_Visualization>
{
public:
    explicit Flow_Visualization(uniflow::Runtime& rt);

    struct Task_Snapshot : uniflow::Task<Flow_Visualization>
    {
        StepResult Entry() override { return Step1_Tick(); }

    private:
        StepResult Step1_Tick();
    } task_snapshot_;
};

// Background render loop: clears, then redraws the ANSI dashboard ~25 fps from
// g_snap until 'quit' is set (Enter on the main thread sets it). Owns stdout.
void RunConsoleRenderer(std::atomic<bool>& quit);
