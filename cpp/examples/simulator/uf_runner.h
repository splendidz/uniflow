// uf_runner.h - one simulated worker that laps a track forever.
//
// FEATURE FOCUS: logical (virtual) time. Progress is measured against
// Runtime::clock(), the VirtualClock - so every runner speeds up, slows down,
// and freezes together when the user scales or pauses the sim, with no per-flow
// plumbing. The three steps (Gate -> Move -> Rest) loop, and each loop is one
// lap. All runners run on one pump thread, so reading a peer would be a plain
// member read (no lock) - here each just owns its own lane.
#pragma once

#include "snapshot.h"
#include "uniflow.hpp"

class Flow_Runner : public uniflow::Uniflow<Flow_Runner>
{
public:
    // id selects this runner's dashboard row; move_ms is the virtual-time
    // length of one Move phase (different per runner for a staggered field).
    Flow_Runner(uniflow::Runtime& rt, const char* name, int id, double move_ms);

    // The single looping task. Public so main() can launch it with StartFlow().
    struct Task_Run : uniflow::Task<Flow_Runner>
    {
        // OnEnter re-arms the phase anchor when the task is (re)entered.
        void OnEnter() override;
        StepResult Entry() override { return Step1_Gate(); }

    private:
        uniflow::TimePoint phase_start_{};   // virtual-time anchor for this phase

        StepResult Step1_Gate();   // hold at the start line for a beat
        StepResult Step2_Move();   // advance 0 -> 100% across the track
        StepResult Step3_Rest();   // brief rest, then loop to the next lap

        double   VElapsedMs() const;          // virtual ms since phase_start_
        void     Publish(double percent, const char* step);
    } ctx_run_;

private:
    static constexpr uniflow::Duration kGate{700};   // virtual-time waits (ms)
    static constexpr uniflow::Duration kRest{500};

    uniflow::VirtualClock& clock_;   // the Runtime's logical clock
    int                    id_;
    std::string            name_;
    double                 move_ms_;
    int                    lap_ = 0;
};
