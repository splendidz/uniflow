// uf_traffic_light.cpp - two-phase signal (no left turns). See the header.
#include "uf_traffic_light.h"

#include "globals.h"

using namespace uniflow;

Flow_TrafficLight::Flow_TrafficLight(uniflow::Runtime& rt, int node_id)
    : uniflow::Uniflow<Flow_TrafficLight>(rt, citymap::NodeById(node_id).label)
{
    AddTask(ctx_);
    ctx_.Init(node_id);
}

void Flow_TrafficLight::Task_Signal::Init(int node_id)
{
    node_id_     = node_id;
    start_phase_ = node_id % 2;
    // id-derived spreads so no two intersections share a period; long hold
    // so several cars clear per green.
    green_  = uniflow::Duration((3500 + (node_id * 523) % 2100) * 3 / 2);
    yellow_ = uniflow::Duration(900 + (node_id * 311) % 500);
}

void Flow_TrafficLight::Task_Signal::Publish(city::Axis axis, bool yellow)
{
    city::SignalState s;
    s.axis    = axis;
    s.phase   = city::Movement::Straight; // no left phase in this build
    s.yellow  = yellow;
    s.present = true;
    city::SetSignal(node_id_, s);
}

bool Flow_TrafficLight::Task_Signal::RunPhase(city::Axis axis, const char* label)
{
    bool yellow = timer_.Passed(green_);
    Publish(axis, yellow);
    Describe(label, yellow ? " (yellow)" : "");
    return timer_.Passed(green_ + yellow_);
}

StepResult Flow_TrafficLight::Task_Signal::Step_Begin()
{
    timer_.Restart();
    return start_phase_ == 0 ? Next(UF_FN(Step_NsGo)) : Next(UF_FN(Step_EwGo));
}

StepResult Flow_TrafficLight::Task_Signal::Step_NsGo()
{
    if (sim::Stop())
        return Done();
    if (RunPhase(city::Axis::NS, "NS"))
    {
        timer_.Restart();
        return Next(UF_FN(Step_EwGo));
    }
    return Stay();
}

StepResult Flow_TrafficLight::Task_Signal::Step_EwGo()
{
    if (sim::Stop())
        return Done();
    if (RunPhase(city::Axis::EW, "EW"))
    {
        timer_.Restart();
        return Next(UF_FN(Step_NsGo));
    }
    return Stay();
}
