// map.cpp - the fixed node/edge tables. See map.h for the layout diagram.
#include "map.h"

namespace citymap
{

namespace
{

// id, gx, gy, kind, label
const std::vector<Node> kNodes = {
    { 0, 1.0, 1.0, NodeKind::FourWay,  "X"  },
    { 1, 1.0, 0.0, NodeKind::ThreeWay, "T1" },
    { 2, 0.0, 1.0, NodeKind::ThreeWay, "T3" },
    { 3, 2.0, 1.0, NodeKind::ThreeWay, "T4" },
    { 4, 1.0, 2.0, NodeKind::ThreeWay, "T5" },
    { 5, 0.0, 0.0, NodeKind::Corner,   ""   },
    { 6, 2.0, 0.0, NodeKind::Corner,   ""   },
    { 7, 2.0, 2.0, NodeKind::Corner,   ""   },
    { 8, 0.0, 2.0, NodeKind::Corner,   ""   },
};

// 12 edges: 8 ring + 4 spokes. All orthogonal.
const std::vector<Edge> kEdges = {
    // perimeter ring (clockwise from top-left corner)
    { 5,  1}, { 1,  6}, { 6,  3}, { 3,  7}, { 7,  4}, { 4,  8}, { 8,  2}, { 2,  5},
    // spokes into the central crossroad
    { 1,  0}, { 4,  0}, { 2,  0}, { 3,  0},
};

} // namespace

const std::vector<Node>& Nodes() { return kNodes; }
const std::vector<Edge>& Edges() { return kEdges; }

const Node& NodeById(int id)
{
    for (const Node& n : kNodes)
        if (n.id == id)
            return n;
    return kNodes.front();
}

std::vector<int> NeighborsOf(int id)
{
    std::vector<int> r;
    for (const Edge& e : kEdges)
    {
        if (e.a == id)
            r.push_back(e.b);
        else if (e.b == id)
            r.push_back(e.a);
    }
    return r;
}

double GridMinX() { return 0.0; }
double GridMaxX() { return 2.0; }
double GridMinY() { return 0.0; }
double GridMaxY() { return 2.0; }

} // namespace citymap
