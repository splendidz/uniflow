// uf_traffic_light.cpp - two-phase signal (no left turns). See the header.
#include "uf_traffic_light.h"

#include "globals.h"

UF_TrafficLight::UF_TrafficLight(uniflow::Runtime& rt, int node_id)
    : uniflow::Uniflow<UF_TrafficLight>(rt, citymap::NodeById(node_id).label),
      node_id_(node_id),
      start_phase_(node_id % 2),
      // id-derived spreads so no two intersections share a period; long hold
      // so several cars clear per green.
      green_(uniflow::Duration((3500 + (node_id * 523) % 2100) * 3 / 2)),
      yellow_(uniflow::Duration(900 + (node_id * 311) % 500))
{
}

void UF_TrafficLight::Publish(city::Axis axis, bool yellow)
{
    city::SignalState s;
    s.axis    = axis;
    s.phase   = city::Movement::Straight; // no left phase in this build
    s.yellow  = yellow;
    s.present = true;
    city::SetSignal(node_id_, s);
}

bool UF_TrafficLight::RunPhase(city::Axis axis, const char* label)
{
    bool yellow = timer_.TimedOut(green_);
    Publish(axis, yellow);
    Describe(label, yellow ? " (yellow)" : "");
    return timer_.TimedOut(green_ + yellow_);
}

UF_TrafficLight::StepResult UF_TrafficLight::OnLight_Begin()
{
    timer_.Restart();
    return start_phase_ == 0 ? UF_NEXT(OnLight_NsGo) : UF_NEXT(OnLight_EwGo);
}

UF_TrafficLight::StepResult UF_TrafficLight::OnLight_NsGo()
{
    if (sim::Stop())
        return Done();
    if (RunPhase(city::Axis::NS, "NS"))
    {
        timer_.Restart();
        return UF_NEXT(OnLight_EwGo);
    }
    return Stay();
}

UF_TrafficLight::StepResult UF_TrafficLight::OnLight_EwGo()
{
    if (sim::Stop())
        return Done();
    if (RunPhase(city::Axis::EW, "EW"))
    {
        timer_.Restart();
        return UF_NEXT(OnLight_NsGo);
    }
    return Stay();
}
