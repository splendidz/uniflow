// uf_view.h - the dashboard renderer, itself a uniflow module.
//
// FEATURE FOCUS: a renderer is just another flow on the same pump. It reads
// every runner's row with plain member access (no lock) because they share the
// thread. Its frame cadence uses a REAL-time UFTimer (default ctor), NOT the
// virtual clock - so when the user pauses the sim (clock frozen), the runners
// stop but the dashboard keeps redrawing and shows [PAUSED].
#pragma once

#include "uniflow.hpp"

class Flow_View : public uniflow::Uniflow<Flow_View>
{
public:
    explicit Flow_View(uniflow::Runtime& rt);

    struct Task_Draw : uniflow::Task<Flow_View>
    {
        void OnEnter() override { fps_.Restart(); }
        StepResult Entry() override { return Step1_Draw(); }

    private:
        uniflow::UFTimer fps_;   // real-clock throttle (keeps drawing while paused)

        StepResult Step1_Draw();
        void       Render();
    } ctx_draw_;

private:
    uniflow::VirtualClock& clock_;   // read scale/frozen state for the header
};
