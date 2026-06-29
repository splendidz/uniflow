// Flows.cs - the three uniflow modules that make up the simulation.
//
//   Flow_TrafficLight (uf_traffic_light.*) - two-phase signal as a module.
//   Flow_Vehicle      (uf_vehicle.*)       - driving state machine as a module.
//   Flow_Visualization(uf_visualization.*) - copies World -> Snapshot each tick.
//
// FEATURE FOCUS:
//   - lock-free shared World on ONE pump: every light, every car, and the
//     snapshot writer are tasks on one Runtime (one pump thread), so they
//     read/write the shared signal + vehicle tables with NO locks.
//   - state-machine-as-module: one Flow_Vehicle = one car = one Task whose steps
//     (Step_Cruise / Step_Wait / Step_Cross / Step_Turn) form the driving FSM.

#nullable enable

using System;
using Uniflow;

namespace Uniflow.CityTraffic
{
    // ========================================================================
    // globals - geometry + cooperative shutdown latch (globals.h)
    // ========================================================================
    internal static class Geo
    {
        // Cooperative stop request: set on shutdown; every module's step loop
        // polls it and returns Done() so WaitUntilIdle can join cleanly.
        public static volatile bool Stop = false;

        // Shared geometry in grid units (1.0 = one road segment). Single source of
        // truth so vehicle physics and the renderer agree where the stop line is.
        public const double StopOffset = 0.32;  // stop line this far before junction
    }

    // ========================================================================
    // Flow_TrafficLight - one autonomous signal per intersection
    // ========================================================================
    //
    // Two-phase cycle (no left turns in this build): each phase gives one axis the
    // green for green, then yellow for yellow, then hands the green to the other
    // axis. NS green -> NS yellow -> EW green -> EW yellow -> (loop). One flow =
    // one signal = one perpetual task.
    internal sealed class Flow_TrafficLight : Module
    {
        public readonly Task_Signal TaskSignal;

        public Flow_TrafficLight(Runtime rt, int nodeId)
            : base(rt, Map.NodeById(nodeId).Label.Length > 0 ? Map.NodeById(nodeId).Label : "T?")
        {
            TaskSignal = new Task_Signal(nodeId);
            AddTask(TaskSignal);
        }

        public sealed class Task_Signal : Task<Flow_TrafficLight>
        {
            private readonly int _nodeId;
            private readonly int _startPhase;
            // id-derived spreads so no two intersections share a period; long hold
            // so several cars clear per green. (ms in C++ -> seconds here)
            private readonly double _green;
            private readonly double _yellow;
            private readonly UFTimer _timer = new UFTimer();

            public Task_Signal(int nodeId)
            {
                _nodeId = nodeId;
                _startPhase = nodeId % 2;
                _green = ((3500 + (nodeId * 523) % 2100) * 3 / 2) / 1000.0;
                _yellow = (900 + (nodeId * 311) % 500) / 1000.0;
            }

            protected override StepResult Entry()
            {
                return Step_Begin();
            }

            private void Publish(Axis axis, bool yellow)
            {
                World.SetSignal(_nodeId,
                    new SignalState(axis, Move.Straight, yellow, present: true));
            }

            // Drive the current axis phase; returns true once green+yellow elapsed.
            private bool RunPhase(Axis axis, string label)
            {
                bool yellow = _timer.Passed(_green);
                Publish(axis, yellow);
                Describe(yellow ? label + " (yellow)" : label);
                return _timer.Passed(_green + _yellow);
            }

            private StepResult Step_Begin()
            {
                _timer.Restart();
                if (_startPhase == 0)
                {
                    return Next(Step_NsGo);
                }
                return Next(Step_EwGo);
            }

            private StepResult Step_NsGo()
            {
                if (Geo.Stop)
                {
                    return Done();
                }
                if (RunPhase(Axis.Ns, "NS"))
                {
                    _timer.Restart();
                    return Next(Step_EwGo);
                }
                return Stay();
            }

            private StepResult Step_EwGo()
            {
                if (Geo.Stop)
                {
                    return Done();
                }
                if (RunPhase(Axis.Ew, "EW"))
                {
                    _timer.Restart();
                    return Next(Step_NsGo);
                }
                return Stay();
            }
        }
    }

    // ========================================================================
    // Flow_Vehicle - one car as a uniflow module (uf_vehicle.*)
    // ========================================================================
    //
    // The driving loop is a single perpetual task whose steps form the FSM:
    //   Step_Cruise - accelerate; brake to a stop at the stop line if not green
    //   Step_Wait   - held at the stop line until the signal turns green
    //   Step_Cross  - drive through the junction; pick the next edge (turn) and loop
    //   Step_Turn   - arc through a junction/corner, then re-enter Cruise
    // One flow = one car = one task that OWNS all the kinematic state.
    internal sealed class Flow_Vehicle : Module
    {
        public const double Cruise = 0.55;      // grid units / second (target speed)
        public const double TurnSpeed = 0.26;   // capped speed while turning
        public const double Accel = 0.70;       // grid units / second^2 (accel/brake)
        public const double SafeGap = 0.15;     // min centre-to-centre to a leader
        public const double CornerCut = 0.12;   // arc starts/ends this far from node
        public const double EnterClear = 0.27;  // exit edge must be this clear to enter
        public const double JunctionZone = 0.25; // a car this far past our node is "in"

        public readonly Task_Drive TaskDrive;

        public Flow_Vehicle(Runtime rt, int vid, int frm, int to, double dist0,
                           int r, int g, int b)
            : base(rt, "Flow_Vehicle")
        {
            TaskDrive = new Task_Drive(vid, frm, to, dist0, r, g, b);
            AddTask(TaskDrive);
        }

        public sealed class Task_Drive : Task<Flow_Vehicle>
        {
            private readonly int _id;
            private int _from;
            private int _to;
            private int _next = -1;       // committed turn while crossing
            private int _blink = 0;       // turn signal: -1 left, +1 right
            private double _dist;         // [0,1] along edge from -> to
            private double _speed = 0.0;  // grid units per second
            private double _turnS = 0.0;  // arc parameter [0,1] while turning
            private int _ta = -1;         // arc nodes captured at turn start
            private int _tnode = -1;
            private int _tc = -1;
            private double _px = 0.0;     // published centreline position
            private double _py = 0.0;
            private double _hx = 1.0;     // heading (normalised grid)
            private double _hy = 0.0;
            private readonly int _r;
            private readonly int _g;
            private readonly int _b;
            private long _last;           // Stopwatch ticks of last dt() read
            private readonly Random _rng;

            public Task_Drive(int vid, int frm, int to, double dist0,
                             int r, int g, int b)
            {
                _id = vid;
                _from = frm;
                _to = to;
                _dist = dist0;
                _r = r;
                _g = g;
                _b = b;
                _last = System.Diagnostics.Stopwatch.GetTimestamp();
                unchecked
                {
                    _rng = new Random((int)(uint)(vid * 2654435761 + 12345));
                }
                UpdateHeading();
            }

            protected override StepResult Entry()
            {
                return Step_Cruise();
            }

            // ----- kinematics helpers -----

            // Real elapsed seconds since the last call, clamped. Mirrors Python's
            // time.perf_counter() delta; uses Stopwatch timestamps here.
            private double Dt()
            {
                long now = System.Diagnostics.Stopwatch.GetTimestamp();
                double d = (now - _last) / (double)System.Diagnostics.Stopwatch.Frequency;
                _last = now;
                if (d < 0.0)
                {
                    d = 0.0;
                }
                if (d > 0.1)   // clamp first/large gap so position never jumps
                {
                    d = 0.1;
                }
                return d;
            }

            private void ApproachSpeed(double target, double dt)
            {
                double step = Flow_Vehicle.Accel * dt;
                if (_speed < target)
                {
                    _speed = Math.Min(target, _speed + step);
                }
                else
                {
                    _speed = Math.Max(target, _speed - step);
                }
                if (_speed < 0.0)
                {
                    _speed = 0.0;
                }
            }

            private void UpdateHeading()
            {
                Node a = Map.NodeById(_from);
                Node b = Map.NodeById(_to);
                double dx = b.Gx - a.Gx;
                double dy = b.Gy - a.Gy;
                double length = Math.Sqrt(dx * dx + dy * dy);
                if (length > 1e-9)
                {
                    _hx = dx / length;
                    _hy = dy / length;
                }
            }

            private void UpdateEdgePos()
            {
                Node a = Map.NodeById(_from);
                Node b = Map.NodeById(_to);
                _px = a.Gx + (b.Gx - a.Gx) * _dist;
                _py = a.Gy + (b.Gy - a.Gy) * _dist;
                UpdateHeading();
            }

            private void Publish()
            {
                var s = new VehicleState
                {
                    From = _from,
                    To = _to,
                    Dist = _dist,
                    Gx = _px,
                    Gy = _py,
                    Dx = _hx,
                    Dy = _hy,
                    Blink = _blink,
                    Active = true,
                    R = _r,
                    G = _g,
                    B = _b,
                };
                World.SetVehicle(_id, s);
            }

            // Distance to the nearest car ahead on this edge / our continued path.
            private double GapAhead(int nxt)
            {
                VehicleState[] vs = World.Vehicles();
                double best = 1e9;
                for (int i = 0; i < vs.Length; i++)
                {
                    VehicleState o = vs[i];
                    if (i == _id || !o.Active)
                    {
                        continue;
                    }
                    if (o.From == _from && o.To == _to)
                    {
                        if (o.Dist > _dist)
                        {
                            best = Math.Min(best, o.Dist - _dist);
                        }
                    }
                    else if (o.From == _to)
                    {
                        // A car that has crossed our node: our path leader on the
                        // next edge, or any car still inside the junction zone.
                        bool myPath = nxt >= 0 && o.To == nxt;
                        if (myPath || o.Dist < Flow_Vehicle.JunctionZone)
                        {
                            best = Math.Min(best, (1.0 - _dist) + o.Dist);
                        }
                    }
                }
                return best;
            }

            // How far the nearest car on the exit edge (from=_to, to=nxt) has gone.
            private double NextEdgeRoom(int nxt)
            {
                VehicleState[] vs = World.Vehicles();
                double best = 1e9;
                for (int i = 0; i < vs.Length; i++)
                {
                    VehicleState o = vs[i];
                    if (i == _id)
                    {
                        continue;
                    }
                    if (o.Active && o.From == _to && o.To == nxt)
                    {
                        best = Math.Min(best, o.Dist);
                    }
                }
                return best;
            }

            // -1 left, 0 straight, +1 right for a candidate next node n.
            private int TurnDir(int n)
            {
                Node nt = Map.NodeById(_to);
                Node nn = Map.NodeById(n);
                double ox = nn.Gx - nt.Gx;
                double oy = nn.Gy - nt.Gy;
                double length = Math.Sqrt(ox * ox + oy * oy);
                if (length > 1e-9)
                {
                    ox /= length;
                    oy /= length;
                }
                double dot = _hx * ox + _hy * oy;
                if (dot > 0.8)
                {
                    return 0;                       // straight
                }
                double cz = _hx * oy - _hy * ox;    // y-down screen
                return cz > 0 ? 1 : -1;             // +1 right, -1 left
            }

            private int PickNextNode()
            {
                int prev = _from;
                int at = _to;
                Node pa = Map.NodeById(prev);
                Node pat = Map.NodeById(at);

                double inx = pat.Gx - pa.Gx;
                double iny = pat.Gy - pa.Gy;
                double li = Math.Sqrt(inx * inx + iny * iny);
                if (li > 1e-9)
                {
                    inx /= li;
                    iny /= li;
                }

                var cand = new System.Collections.Generic.List<int>();
                var candTurn = new System.Collections.Generic.List<int>(); // -1/0/+1
                foreach (int nb in Map.NeighborsOf(at))
                {
                    if (nb == prev)
                    {
                        continue;           // no U-turn
                    }
                    Node n = Map.NodeById(nb);
                    double ox = n.Gx - pat.Gx;
                    double oy = n.Gy - pat.Gy;
                    double lo = Math.Sqrt(ox * ox + oy * oy);
                    if (lo > 1e-9)
                    {
                        ox /= lo;
                        oy /= lo;
                    }
                    double dot = inx * ox + iny * oy;
                    if (dot < -0.5)
                    {
                        continue;           // would be a U-turn
                    }
                    double cz = inx * oy - iny * ox;
                    cand.Add(nb);
                    candTurn.Add(dot > 0.8 ? 0 : (cz > 0 ? 1 : -1));
                }

                if (cand.Count == 0)
                {
                    return prev;   // dead-end fallback (should not happen here)
                }

                // No left turns in this build: drop left exits when a straight/right
                // option exists (keep them only as a last resort).
                var kc = new System.Collections.Generic.List<int>();
                var kt = new System.Collections.Generic.List<int>();
                for (int k = 0; k < cand.Count; k++)
                {
                    if (candTurn[k] != -1)
                    {
                        kc.Add(cand[k]);
                        kt.Add(candTurn[k]);
                    }
                }
                if (kc.Count > 0)
                {
                    cand = kc;
                    candTurn = kt;
                }

                // 60%: follow the car ahead on this edge, matching its turn, so cars
                // form platoons and the map looks busier.
                bool hasLeader = false;
                int leaderTurn = 0;
                double bestDist = 1e9;
                VehicleState[] vs = World.Vehicles();
                for (int i = 0; i < vs.Length; i++)
                {
                    VehicleState o = vs[i];
                    if (i == _id)
                    {
                        continue;
                    }
                    if (o.Active && o.From == _from && o.To == _to &&
                        o.Dist > _dist && o.Dist < bestDist)
                    {
                        bestDist = o.Dist;
                        leaderTurn = o.Blink;
                        hasLeader = true;
                    }
                }
                if (hasLeader && _rng.NextDouble() < 0.6)
                {
                    for (int k = 0; k < cand.Count; k++)
                    {
                        if (candTurn[k] == leaderTurn)
                        {
                            return cand[k];
                        }
                    }
                }

                return cand[_rng.Next(cand.Count)];
            }

            // ----- the driving state machine -----

            private StepResult Step_Cruise()
            {
                if (Geo.Stop)
                {
                    return Done();
                }

                // Decide the route for this edge up front so we know which signal
                // phase to obey and can signal the turn on the approach.
                if (_next < 0)
                {
                    _next = PickNextNode();
                }

                double dt = Dt();
                Node a = Map.NodeById(_from);
                Node b = Map.NodeById(_to);
                Axis ax = World.AxisOfEdge(a, b);
                int td = TurnDir(_next);
                bool junction = b.Kind != NodeKind.Corner;
                _blink = junction ? td : 0;

                SignalState sig = World.GetSignal(_to);
                bool mayGo = World.MayProceed(sig, ax, td, World.AxisSingleEnded(_to, ax));
                bool exitBlocked = NextEdgeRoom(_next) < Flow_Vehicle.EnterClear;

                double stopline = 1.0 - Geo.StopOffset;
                bool needStop = !mayGo || exitBlocked;

                // Nearest point we must not pass: stop line (if stopping) or the safe
                // distance behind the leader on this edge - whichever is closer.
                double bind = 1e9;
                if (needStop)
                {
                    bind = Math.Min(bind, stopline);
                }
                double leadGap = GapAhead(-1);
                if (leadGap < 1e8)
                {
                    bind = Math.Min(bind, _dist + (leadGap - Flow_Vehicle.SafeGap));
                }

                double clearance = bind - _dist;
                double target = clearance <= 0.0
                    ? 0.0
                    : Math.Min(Flow_Vehicle.Cruise, Math.Sqrt(2.0 * Flow_Vehicle.Accel * clearance));

                double prev = _dist;
                ApproachSpeed(target, dt);
                _dist += _speed * dt;
                if (_dist > bind)          // do not overshoot the constraint
                {
                    _dist = Math.Max(prev, bind);
                    _speed = 0.0;
                }
                UpdateEdgePos();
                Publish();

                if (_dist >= stopline - 1e-9)
                {
                    if (mayGo && !exitBlocked)
                    {
                        return Next(Step_Cross);
                    }
                    _speed = 0.0;
                    return Next(Step_Wait);
                }

                Describe("cruise");
                return Stay();
            }

            private StepResult Step_Wait()
            {
                if (Geo.Stop)
                {
                    return Done();
                }

                Dt();   // keep the clock fresh so Cross starts with a small dt
                _speed = 0.0;
                UpdateEdgePos();
                Publish();

                if (_next < 0)
                {
                    _next = PickNextNode();
                }

                Node a = Map.NodeById(_from);
                Node b = Map.NodeById(_to);
                Axis ax = World.AxisOfEdge(a, b);
                int td = TurnDir(_next);
                bool junction = b.Kind != NodeKind.Corner;
                _blink = junction ? td : 0;

                SignalState sig = World.GetSignal(_to);
                if (World.MayProceed(sig, ax, td, World.AxisSingleEnded(_to, ax)) &&
                    NextEdgeRoom(_next) >= Flow_Vehicle.EnterClear)
                {
                    return Next(Step_Cross);
                }

                Describe("wait");
                return Stay();
            }

            private StepResult Step_Cross()
            {
                if (Geo.Stop)
                {
                    return Done();
                }

                if (_next < 0)
                {
                    _next = PickNextNode();
                }

                double dt = Dt();

                // Is this move a turn? Compare incoming heading with the outgoing.
                Node nt = Map.NodeById(_to);
                Node nn = Map.NodeById(_next);
                double ox = nn.Gx - nt.Gx;
                double oy = nn.Gy - nt.Gy;
                double lo = Math.Sqrt(ox * ox + oy * oy);
                if (lo > 1e-9)
                {
                    ox /= lo;
                    oy /= lo;
                }
                double turnDot = _hx * ox + _hy * oy;  // 1 straight, 0 = 90 deg turn
                bool turning = turnDot < 0.8;
                // A turn pivots early: hand off to the arc CornerCut before node.
                double handoff = turning ? (1.0 - Flow_Vehicle.CornerCut) : 1.0;

                double cz = _hx * oy - _hy * ox;        // turn direction (y-down)
                bool atJunction = Map.NodeById(_to).Kind != NodeKind.Corner;
                _blink = (turning && atJunction) ? (cz > 0 ? 1 : -1) : 0;

                double leadGap = GapAhead(_next);
                double bind = leadGap < 1e8 ? _dist + (leadGap - Flow_Vehicle.SafeGap) : 1e9;
                double clearance = bind - _dist;
                double target = clearance <= 0.0
                    ? 0.0
                    : Math.Min(Flow_Vehicle.Cruise, Math.Sqrt(2.0 * Flow_Vehicle.Accel * clearance));
                if (turning)
                {
                    target = Math.Min(target, Flow_Vehicle.TurnSpeed);
                }

                double prev = _dist;
                ApproachSpeed(target, dt);
                _dist += _speed * dt;
                if (_dist > bind)
                {
                    _dist = Math.Max(prev, bind);
                    _speed = 0.0;
                }
                if (_dist > handoff)
                {
                    _dist = handoff;   // never roll past the arc's start
                }
                UpdateEdgePos();
                Publish();

                if (turning && _dist >= handoff - 1e-9)
                {
                    _turnS = 0.0;
                    _ta = _from;     // capture arc nodes before mutating
                    _tnode = _to;
                    _tc = _next;
                    return Next(Step_Turn);
                }
                if (!turning && _dist >= 1.0 - 1e-9)
                {
                    _from = _to;
                    _to = _next;
                    _dist -= 1.0;
                    if (_dist < 0.0)
                    {
                        _dist = 0.0;
                    }
                    _next = -1;
                    UpdateHeading();
                    return Next(Step_Cruise);
                }

                Describe("cross");
                return Stay();
            }

            private StepResult Step_Turn()
            {
                if (Geo.Stop)
                {
                    return Done();
                }

                double dt = Dt();
                ApproachSpeed(Flow_Vehicle.TurnSpeed, dt);

                // Quadratic Bezier across the corner, using the nodes captured at
                // entry: from CornerCut before the node, through it, to CornerCut
                // along the next edge.
                Node a = Map.NodeById(_ta);      // edge start
                Node nd = Map.NodeById(_tnode);  // the node (corner)
                Node c = Map.NodeById(_tc);      // next edge end

                double inx = nd.Gx - a.Gx;
                double iny = nd.Gy - a.Gy;
                double li = Math.Sqrt(inx * inx + iny * iny);
                if (li > 1e-9)
                {
                    inx /= li;
                    iny /= li;
                }
                double oux = c.Gx - nd.Gx;
                double ouy = c.Gy - nd.Gy;
                double lo = Math.Sqrt(oux * oux + ouy * ouy);
                if (lo > 1e-9)
                {
                    oux /= lo;
                    ouy /= lo;
                }

                double p0x = nd.Gx - inx * Flow_Vehicle.CornerCut;
                double p0y = nd.Gy - iny * Flow_Vehicle.CornerCut;
                double p1x = nd.Gx;
                double p1y = nd.Gy;
                double p2x = nd.Gx + oux * Flow_Vehicle.CornerCut;
                double p2y = nd.Gy + ouy * Flow_Vehicle.CornerCut;

                // Advance the arc parameter by distance / (approx arc length).
                double approxLen = 1.6 * Flow_Vehicle.CornerCut;
                _turnS += (_speed * dt) / approxLen;
                if (_turnS > 1.0)
                {
                    _turnS = 1.0;
                }
                double s = _turnS;

                _px = (1 - s) * (1 - s) * p0x + 2 * (1 - s) * s * p1x + s * s * p2x;
                _py = (1 - s) * (1 - s) * p0y + 2 * (1 - s) * s * p1y + s * s * p2y;

                double tx = 2 * (1 - s) * (p1x - p0x) + 2 * s * (p2x - p1x);
                double ty = 2 * (1 - s) * (p1y - p0y) + 2 * s * (p2y - p1y);
                double lt = Math.Sqrt(tx * tx + ty * ty);
                if (lt > 1e-9)
                {
                    _hx = tx / lt;
                    _hy = ty / lt;
                }

                // Occupy the destination edge so another car cannot turn into the
                // same spot and overlap us mid-corner.
                _from = _tnode;
                _to = _tc;
                _dist = s * Flow_Vehicle.CornerCut;
                Publish();

                if (_turnS >= 1.0)
                {
                    _dist = Flow_Vehicle.CornerCut;  // emerge just past the node
                    _next = -1;
                    _blink = 0;
                    UpdateHeading();
                    return Next(Step_Cruise);
                }

                Describe("turn");
                return Stay();
            }
        }
    }

    // ========================================================================
    // Flow_Visualization - copies World -> Snapshot each tick (uf_visualization.*)
    // ========================================================================
    //
    // The pump side (Step_Tick) snapshots the shared World (signals + vehicle
    // poses) under World.SnapMu so the render thread sees a consistent frame. That
    // lock is the ONLY cross-thread synchronisation in the demo.
    internal sealed class Flow_Visualization : Module
    {
        public readonly Task_Snapshot TaskSnapshot;

        public Flow_Visualization(Runtime rt)
            : base(rt, "Flow_Visualization")
        {
            TaskSnapshot = new Task_Snapshot();
            AddTask(TaskSnapshot);
        }

        public sealed class Task_Snapshot : Task<Flow_Visualization>
        {
            protected override StepResult Entry()
            {
                return Step_Tick();
            }

            private StepResult Step_Tick()
            {
                if (Geo.Stop)
                {
                    return Done();
                }

                var sigs = new System.Collections.Generic.List<SignalState>();
                foreach (Node n in Map.Nodes)
                {
                    sigs.Add(World.GetSignal(n.Id));
                }

                var cars = new System.Collections.Generic.List<VehicleView>();
                foreach (VehicleState v in World.Vehicles())
                {
                    if (!v.Active)
                    {
                        continue;
                    }
                    cars.Add(new VehicleView(v.Gx, v.Gy, v.Dx, v.Dy, v.Blink,
                                             v.R, v.G, v.B));
                }

                lock (World.SnapMu)
                {
                    World.Snap = new Snapshot { Signals = sigs, Vehicles = cars };
                }
                return Stay();
            }
        }
    }
}
