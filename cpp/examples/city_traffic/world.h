// world.h - shared, mutable simulation state. For now it holds the per-
// intersection traffic-signal table. Every writer/reader runs on the single
// pump thread, so access needs no locks (the framework's core invariant);
// the UI thread only ever sees a copied Snapshot.
#pragma once

#include "map.h"

#include <cstdint>
#include <vector>

namespace city
{

// A junction's right-of-way alternates between two axes. NS = north-south
// (vertical roads), EW = east-west (horizontal roads).
enum class Axis : uint8_t { NS, EW };

// The protected movement class a phase serves. Straight also covers right
// turns; Left is a protected left-turn phase (both opposing approaches turn
// left together, as on a real signal).
enum class Movement : uint8_t { Straight, Left };

enum class LightColor : uint8_t { Red, Yellow, Green }; // rendering only

// The live state of one intersection's signal: which axis is served, with
// which protected movement, and whether it is in the yellow tail. 'present'
// is false for corner bends (no signal).
struct SignalState
{
    Axis     axis    = Axis::NS;
    Movement phase   = Movement::Straight;
    bool     yellow  = false;
    bool     present = false;
};

// Axis of the orthogonal road between two nodes (same row -> EW, else NS).
Axis AxisOfEdge(const citymap::Node& from, const citymap::Node& to);

// May a car on the 'approach' axis, making the move 'turnDir' (-1 left,
// 0 straight, +1 right), enter the junction now? Right turns go on the
// straight phase of their own axis - and, when 'axisSingleEnded' (a T-junction
// stem with no opposing approach), on that axis's left phase too. Corners
// (no signal) always allow it.
bool MayProceed(const SignalState& s, Axis approach, int turnDir,
                bool axisSingleEnded);

// True if 'axis' has only one incident road at 'node' (the stem of a T).
bool AxisSingleEnded(int node, Axis axis);

// Rendering helpers: the colour of an approach's straight/right signal and
// of its separate left-turn signal.
LightColor StraightLight(const SignalState& s, Axis approach);
LightColor LeftLight(const SignalState& s, Axis approach);

// Shared signal table, indexed by node id. Pump-thread access only.
void        SetSignal(int node_id, SignalState s);
SignalState GetSignal(int node_id);

// ----- vehicles -----
// Every vehicle publishes its position here each tick so peers can see each
// other (car-ahead / safety-distance logic, step 4) and the renderer can draw
// them. Pump-thread access only; the UI reads a copied Snapshot.
struct VehicleState
{
    int     from   = -1;  // directed edge: travelling from this node...
    int     to     = -1;  // ...toward this node
    double  dist   = 0.0; // [0,1] along that edge (used for car-ahead spacing)
    double  gx     = 0.0; // centreline world position (grid) - the renderer
    double  gy     = 0.0; // uses this directly so turns can follow a curve
    double  dx     = 1.0; // heading (grid units, normalised)
    double  dy     = 0.0;
    int     blink  = 0;   // turn signal: -1 left, 0 none, +1 right
    bool    active = false;
    uint8_t r      = 200;
    uint8_t g      = 200;
    uint8_t b      = 200;
};

void                             InitVehicles(int n); // size the table once
void                             SetVehicle(int id, const VehicleState& s);
const std::vector<VehicleState>& Vehicles();

} // namespace city
