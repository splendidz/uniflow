// world.cpp - backing storage and helpers for the shared signal table.
#include "world.h"

#include <vector>

namespace city
{

namespace
{
// Sized to the node count on first use; one slot per node id. Function-local
// static so initialisation order against the map table is well defined.
std::vector<SignalState>& table()
{
    static std::vector<SignalState> t(citymap::Nodes().size());
    return t;
}
} // namespace

Axis AxisOfEdge(const citymap::Node& from, const citymap::Node& to)
{
    // Roads are orthogonal: equal grid-y means a horizontal (east-west) road.
    return from.gy == to.gy ? Axis::EW : Axis::NS;
}

bool MayProceed(const SignalState& s, Axis approach, int turnDir,
                bool axisSingleEnded)
{
    if (!s.present)
        return true;        // corner bend - no signal
    if (s.yellow)
        return false;       // no new entries on yellow
    if (approach != s.axis)
        return false;       // cross axis is stopped
    if (turnDir < 0)
        return s.phase == Movement::Left;     // protected left
    if (turnDir > 0)
        // Right: on the straight phase, plus this axis's left phase when the
        // axis is single-ended (a T stem), where left and right never cross.
        return s.phase == Movement::Straight || axisSingleEnded;
    return s.phase == Movement::Straight;     // straight
}

bool AxisSingleEnded(int node, Axis axis)
{
    const citymap::Node& n     = citymap::NodeById(node);
    int                  count = 0;
    for (int nb : citymap::NeighborsOf(node))
        if (AxisOfEdge(n, citymap::NodeById(nb)) == axis)
            ++count;
    return count == 1;
}

LightColor StraightLight(const SignalState& s, Axis approach)
{
    if (!s.present)
        return LightColor::Green;
    if (approach != s.axis || s.phase != Movement::Straight)
        return LightColor::Red;
    return s.yellow ? LightColor::Yellow : LightColor::Green;
}

LightColor LeftLight(const SignalState& s, Axis approach)
{
    if (!s.present)
        return LightColor::Green;
    if (approach != s.axis || s.phase != Movement::Left)
        return LightColor::Red;
    return s.yellow ? LightColor::Yellow : LightColor::Green;
}

void SetSignal(int node_id, SignalState s)
{
    auto& t = table();
    if (node_id >= 0 && node_id < static_cast<int>(t.size()))
        t[node_id] = s;
}

SignalState GetSignal(int node_id)
{
    auto& t = table();
    if (node_id >= 0 && node_id < static_cast<int>(t.size()))
        return t[node_id];
    return {};
}

namespace
{
std::vector<VehicleState>& vtable()
{
    static std::vector<VehicleState> v;
    return v;
}
} // namespace

void InitVehicles(int n) { vtable().assign(n, VehicleState{}); }

void SetVehicle(int id, const VehicleState& s)
{
    auto& v = vtable();
    if (id >= 0 && id < static_cast<int>(v.size()))
        v[id] = s;
}

const std::vector<VehicleState>& Vehicles() { return vtable(); }

} // namespace city
