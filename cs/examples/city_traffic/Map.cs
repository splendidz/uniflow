// Map.cs - immutable city road network (C# port of map.cpp / map.h).
//
// Layout (grid units, y grows downward):
//
//     C_TL ---- T1 ---- C_TR
//      |        |         |
//      T3 ----- X  ------ T4
//      |        |         |
//     C_BL ---- T5 ---- C_BR
//
//   X       : four-way crossroad, degree 4
//   T1..T5  : three-way junctions, degree 3
//   C_*     : corner bends, degree 2 (no turn decision)

#nullable enable

using System.Collections.Generic;

namespace Uniflow.CityTraffic
{
    internal enum NodeKind
    {
        Corner = 0,    // degree-2 bend, no turn decision
        ThreeWay = 1,  // T-junction
        FourWay = 2    // crossroad
    }

    internal sealed class Node
    {
        public int Id;
        public double Gx;
        public double Gy;
        public NodeKind Kind;
        public string Label = "";

        public Node(int id, double gx, double gy, NodeKind kind, string label)
        {
            Id = id;
            Gx = gx;
            Gy = gy;
            Kind = kind;
            Label = label;
        }
    }

    // Immutable city graph: the node table, the edge list, and the precomputed
    // neighbour adjacency. Plain static data shared read-only by every flow.
    internal static class Map
    {
        // id, gx, gy, kind, label
        public static readonly Node[] Nodes =
        {
            new Node(0, 1.0, 1.0, NodeKind.FourWay, "X"),
            new Node(1, 1.0, 0.0, NodeKind.ThreeWay, "T1"),
            new Node(2, 0.0, 1.0, NodeKind.ThreeWay, "T3"),
            new Node(3, 2.0, 1.0, NodeKind.ThreeWay, "T4"),
            new Node(4, 1.0, 2.0, NodeKind.ThreeWay, "T5"),
            new Node(5, 0.0, 0.0, NodeKind.Corner, ""),
            new Node(6, 2.0, 0.0, NodeKind.Corner, ""),
            new Node(7, 2.0, 2.0, NodeKind.Corner, ""),
            new Node(8, 0.0, 2.0, NodeKind.Corner, ""),
        };

        // 12 edges: 8 perimeter ring + 4 spokes into the central crossroad.
        public static readonly (int A, int B)[] Edges =
        {
            (5, 1), (1, 6), (6, 3), (3, 7), (7, 4), (4, 8), (8, 2), (2, 5),
            (1, 0), (4, 0), (2, 0), (3, 0),
        };

        private static readonly Dictionary<int, Node> NodeByIdMap = BuildNodeMap();
        private static readonly Dictionary<int, List<int>> NeighborMap = BuildNeighbors();

        private static Dictionary<int, Node> BuildNodeMap()
        {
            var m = new Dictionary<int, Node>();
            foreach (Node n in Nodes)
            {
                m[n.Id] = n;
            }
            return m;
        }

        private static Dictionary<int, List<int>> BuildNeighbors()
        {
            var m = new Dictionary<int, List<int>>();
            foreach (Node n in Nodes)
            {
                m[n.Id] = new List<int>();
            }
            foreach (var e in Edges)
            {
                m[e.A].Add(e.B);
                m[e.B].Add(e.A);
            }
            return m;
        }

        public static Node NodeById(int nid)
        {
            return NodeByIdMap.TryGetValue(nid, out Node? n) ? n : Nodes[0];
        }

        public static IReadOnlyList<int> NeighborsOf(int nid)
        {
            return NeighborMap.TryGetValue(nid, out List<int>? l)
                ? l
                : (IReadOnlyList<int>)System.Array.Empty<int>();
        }
    }
}
