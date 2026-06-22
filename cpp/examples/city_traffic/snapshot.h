// snapshot.h - pump -> UI hand-off. Flow_Visualization writes g_snap under
// g_snap_mu each tick; the render thread reads via ReadSnapshot(). This mutex
// is the only cross-thread synchronisation in the demo.
#pragma once

#include "world.h"

#include <cstdint>
#include <mutex>
#include <vector>

// Render-ready vehicle: grid-space centre (on the road centreline), heading,
// and colour. The renderer applies the lane offset and orientation.
struct VehicleView
{
    double  gx, gy; // grid position on the road centreline
    double  dx, dy; // heading (grid units, normalised)
    int     blink;  // turn signal: -1 left, 0 none, +1 right
    uint8_t r, g, b;
};

struct Snapshot
{
    // Signal state per node id (corners carry a default, unused state).
    std::vector<city::SignalState> signals;
    std::vector<VehicleView>       vehicles;
};

extern Snapshot   g_snap;
extern std::mutex g_snap_mu;

Snapshot ReadSnapshot();
