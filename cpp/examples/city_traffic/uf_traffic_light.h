// uf_traffic_light.h - one autonomous traffic signal per intersection.
//
// Two-phase cycle (no left turns in this build): each phase gives one axis the
// green for green_, then yellow for yellow_, then hands the green to the other
// axis. Straight and right turns go together on the green.
//
//   NS green -> NS yellow -> EW green -> EW yellow -> (loop)
#pragma once

#include "uniflow.hpp"
#include "world.h"

class UF_TrafficLight : public uniflow::Uniflow<UF_TrafficLight>
{
    UF_UNIFLOW_IMPLEMENT(UF_TrafficLight);

public:
    UF_TrafficLight(uniflow::Runtime& rt, int node_id);

    StepResult OnLight_Begin();

private:
    StepResult OnLight_NsGo();
    StepResult OnLight_EwGo();

    bool RunPhase(city::Axis axis, const char* label);
    void Publish(city::Axis axis, bool yellow);

    int               node_id_;
    int               start_phase_; // 0 or 1, staggers the initial phase
    uniflow::Duration green_;
    uniflow::Duration yellow_;
    uniflow::UFTimer  timer_;
};
