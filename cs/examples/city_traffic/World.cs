// World.cs - shared, mutable simulation state (C# port of world.cpp / world.h)
// plus the pump -> UI snapshot hand-off (snapshot.cpp / snapshot.h).
//
// REFERENCE NOTE: there are NO locks on the World, on purpose. Every
// Flow_TrafficLight, every Flow_Vehicle, and Flow_Visualization are modules on
// the SAME Runtime, so they advance on the one pump thread round-robin. A writer
// and a reader never overlap - the core uniflow guarantee. The UI thread only
// ever sees the copied Snapshot, guarded by the one cross-thread lock.

#nullable enable

using System.Collections.Generic;

namespace Uniflow.CityTraffic
{
    internal enum Axis
    {
        Ns = 0,  // north-south (vertical roads)
        Ew = 1   // east-west (horizontal roads)
    }

    internal enum Move
    {
        Straight = 0,
        Left = 1
    }

    internal enum LightColor
    {
        Red = 0,
        Yellow = 1,
        Green = 2
    }

    // Live state of one intersection's signal. Present=false for corners.
    internal sealed class SignalState
    {
        public Axis Axis;
        public Move Phase;
        public bool Yellow;
        public bool Present;

        public SignalState(Axis axis = Axis.Ns, Move phase = Move.Straight,
                           bool yellow = false, bool present = false)
        {
            Axis = axis;
            Phase = phase;
            Yellow = yellow;
            Present = present;
        }
    }

    // Every vehicle publishes its pose here each tick so peers can see each other
    // (car-ahead spacing) and the renderer can draw them.
    internal sealed class VehicleState
    {
        public int From = -1;
        public int To = -1;
        public double Dist = 0.0;
        public double Gx = 0.0;
        public double Gy = 0.0;
        public double Dx = 1.0;
        public double Dy = 0.0;
        public int Blink = 0;     // turn signal: -1 left, 0 none, +1 right
        public bool Active = false;
        public int R = 200;
        public int G = 200;
        public int B = 200;
    }

    // The snapshot's per-vehicle view (pump -> UI hand-off).
    internal sealed class VehicleView
    {
        public double Gx;
        public double Gy;
        public double Dx;
        public double Dy;
        public int Blink;
        public int R;
        public int G;
        public int B;

        public VehicleView(double gx, double gy, double dx, double dy,
                           int blink, int r, int g, int b)
        {
            Gx = gx;
            Gy = gy;
            Dx = dx;
            Dy = dy;
            Blink = blink;
            R = r;
            G = g;
            B = b;
        }
    }

    // One frame handed from the pump to the render thread.
    internal sealed class Snapshot
    {
        public List<SignalState> Signals = new List<SignalState>();
        public List<VehicleView> Vehicles = new List<VehicleView>();
    }

    // Shared signal table (indexed by node id) and the vehicle table. Pump-only:
    // no locks. The snapshot is the only structure crossing to the render thread.
    internal static class World
    {
        private static readonly SignalState[] Signals = NewSignals();
        private static VehicleState[] _vehicles = System.Array.Empty<VehicleState>();

        // The single cross-thread hand-off: written under SnapMu by the pump,
        // read under SnapMu by the render thread. The ONLY lock in the demo.
        public static readonly object SnapMu = new object();
        public static Snapshot Snap = new Snapshot();

        private static SignalState[] NewSignals()
        {
            var s = new SignalState[Map.Nodes.Length];
            for (int i = 0; i < s.Length; i++)
            {
                s[i] = new SignalState();
            }
            return s;
        }

        // ----- geometry / signal helpers -----

        public static Axis AxisOfEdge(Node a, Node b)
        {
            // Roads are orthogonal: equal grid-y means a horizontal (E-W) road.
            return a.Gy == b.Gy ? Axis.Ew : Axis.Ns;
        }

        public static bool MayProceed(SignalState s, Axis approach, int turnDir,
                                      bool axisSingleEnded)
        {
            if (!s.Present)
            {
                return true;             // corner bend - no signal
            }
            if (s.Yellow)
            {
                return false;            // no new entries on yellow
            }
            if (approach != s.Axis)
            {
                return false;            // cross axis is stopped
            }
            if (turnDir < 0)
            {
                return s.Phase == Move.Left;            // protected left
            }
            if (turnDir > 0)
            {
                return s.Phase == Move.Straight || axisSingleEnded;
            }
            return s.Phase == Move.Straight;            // straight
        }

        public static bool AxisSingleEnded(int nodeId, Axis axis)
        {
            Node n = Map.NodeById(nodeId);
            int count = 0;
            foreach (int nb in Map.NeighborsOf(nodeId))
            {
                if (AxisOfEdge(n, Map.NodeById(nb)) == axis)
                {
                    count++;
                }
            }
            return count == 1;
        }

        public static LightColor StraightLight(SignalState s, Axis approach)
        {
            if (!s.Present)
            {
                return LightColor.Green;
            }
            if (approach != s.Axis || s.Phase != Move.Straight)
            {
                return LightColor.Red;
            }
            return s.Yellow ? LightColor.Yellow : LightColor.Green;
        }

        // ----- signal table -----

        public static void SetSignal(int nodeId, SignalState s)
        {
            if (nodeId >= 0 && nodeId < Signals.Length)
            {
                Signals[nodeId] = s;
            }
        }

        public static SignalState GetSignal(int nodeId)
        {
            if (nodeId >= 0 && nodeId < Signals.Length)
            {
                return Signals[nodeId];
            }
            return new SignalState();
        }

        // ----- vehicle table -----

        public static void InitVehicles(int n)
        {
            _vehicles = new VehicleState[n];
            for (int i = 0; i < n; i++)
            {
                _vehicles[i] = new VehicleState();
            }
        }

        public static void SetVehicle(int vid, VehicleState s)
        {
            if (vid >= 0 && vid < _vehicles.Length)
            {
                _vehicles[vid] = s;
            }
        }

        public static VehicleState[] Vehicles()
        {
            return _vehicles;
        }
    }
}
