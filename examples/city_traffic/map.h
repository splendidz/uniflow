// map.h - immutable city road network. Nodes are intersections (junction)
// or corner bends; edges are two-lane bidirectional roads (oncoming traffic,
// no lane change). The network is fixed at startup and never mutated.
//
// Layout (grid units, y grows downward):
//
//     C_TL ---- T1 ---- C_TR
//      |        |         |
//      T3 ----- X  ------ T4
//      |        |         |
//     C_BL ---- T5 ---- C_BR
//
//   X       : four-way crossroad (sageori), degree 4
//   T1..T5  : three-way junctions (samgeori), degree 3
//   C_*     : corner bends, degree 2 (no turn decision, cars pass through)
//
// Every road is orthogonal and the perimeter is a closed ring, so there are
// no dead ends - a fixed set of vehicles can circulate indefinitely.
#pragma once

#include <cstdint>
#include <vector>

namespace citymap
{

enum class NodeKind : uint8_t
{
    Corner,   // degree-2 bend, no turn decision
    ThreeWay, // T-junction (samgeori)
    FourWay   // crossroad (sageori)
};

struct Node
{
    int         id;
    double      gx;    // grid x
    double      gy;    // grid y
    NodeKind    kind;
    const char* label; // ASCII label for the renderer ("X1", "T1", or "")
};

struct Edge
{
    int a; // node id
    int b; // node id
};

// The fixed network.
const std::vector<Node>& Nodes();
const std::vector<Edge>& Edges();

// Linear lookup (the graph is tiny - a dozen nodes).
const Node& NodeById(int id);

// Ids of the nodes directly connected to 'id' by a road.
std::vector<int> NeighborsOf(int id);

// Grid bounds for the renderer's coordinate mapping.
double GridMinX();
double GridMaxX();
double GridMinY();
double GridMaxY();

} // namespace citymap
