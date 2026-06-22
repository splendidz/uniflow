// uf_traffic_light.h - one autonomous traffic signal per intersection.
//
// Two-phase cycle (no left turns in this build): each phase gives one axis the
// green for green_, then yellow for yellow_, then hands the green to the other
// axis. Straight and right turns go together on the green.
//
//   NS green -> NS yellow -> EW green -> EW yellow -> (loop)
//
// One flow = one signal = one perpetual task. The task owns the cycle state and
// its steps; the flow is just the shell that names the module and binds it.
#pragma once

#include "uniflow.hpp"
#include "world.h"

class Flow_TrafficLight : public uniflow::Uniflow<Flow_TrafficLight>
{
public:
    Flow_TrafficLight(uniflow::Runtime& rt, int node_id);

    // The single perpetual Signal task.
    struct Task_Signal : uniflow::Task<Flow_TrafficLight>
    {
        void Init(int node_id);                  // called once from the flow ctor
        StepResult Entry() override { return Step_Begin(); }

    private:
        StepResult Step_Begin();
        StepResult Step_NsGo();
        StepResult Step_EwGo();

        bool RunPhase(city::Axis axis, const char* label);
        void Publish(city::Axis axis, bool yellow);

        int               node_id_    = 0;
        int               start_phase_ = 0;      // 0 or 1, staggers the initial phase
        uniflow::Duration green_{};
        uniflow::Duration yellow_{};
        uniflow::UFTimer  timer_;
    } ctx_;
};
