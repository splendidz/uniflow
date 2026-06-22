"""city_traffic.py - a city-driving simulation built entirely on uniflow.

Python port of cpp/examples/city_traffic/ (CONSOLE renderer only; the C++ build
also has a Win32 back-end which has no Python counterpart). Mirrors:
  map.cpp/.h            -> NODES / EDGES tables + helpers (Map section)
  world.cpp/.h          -> SignalState / VehicleState + lock-free shared World
  globals.h             -> geometry constants + cooperative g_stop latch
  uf_traffic_light.*    -> Flow_TrafficLight (two-phase signal as a module)
  uf_vehicle.*          -> Flow_Vehicle (driving state machine as a module)
  uf_visualization.*    -> Flow_Visualization (World -> Snapshot each tick)
  uf_visualization_console.cpp -> draw_console() ANSI raster renderer
  app.h / main.cpp      -> App + main (stdin blocks; Enter / EOF quits)

FEATURE FOCUS:
  - lock-free shared World on one pump: every traffic-light, every vehicle, and
    the snapshot writer are modules on ONE Runtime (one pump thread), so they
    read/write the shared signal + vehicle tables with NO locks. The only
    cross-thread boundary is the Snapshot the background renderer reads.
  - state-machine-as-module: one Flow_Vehicle = one car = one Task whose steps
    (Step_Cruise / Step_Wait / Step_Cross / Step_Turn) form the driving FSM.
  - console raster renderer: the grid map, per-approach signal lamps, and the
    moving fleet are rasterised onto a character canvas with 24-bit colour.

NOTE ON UNITS: the C++ port uses milliseconds for signal phases; Python's
clock is in SECONDS, so the same durations are expressed in seconds here.
"""

import math
import os
import random
import sys
import threading
import time

# --- import shim: make the python/ package dir importable, then sibling console
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import uniflow  # noqa: E402
import console  # noqa: E402  (same dir as this file)


# ============================================================================
# globals - geometry + cooperative shutdown latch (globals.h)
# ============================================================================

# Cooperative stop request: set on shutdown; every module's step loop polls it
# and returns Done() so WaitUntilIdle can join cleanly.
g_stop = threading.Event()

# Shared geometry in grid units (1.0 = one road segment). Single source of truth
# so vehicle physics and the renderer agree where the stop line is.
GEO_STOP_OFFSET = 0.32  # stop line this far before the junction
GEO_LANE_HALF = 0.10    # lane centre offset from the road centre (unused console)
GEO_ROAD_WIDTH = 0.38   # drawn road width


# ============================================================================
# map - immutable city road network (map.cpp / map.h)
# ============================================================================
#
# Layout (grid units, y grows downward):
#
#     C_TL ---- T1 ---- C_TR
#      |        |         |
#      T3 ----- X  ------ T4
#      |        |         |
#     C_BL ---- T5 ---- C_BR
#
#   X       : four-way crossroad, degree 4
#   T1..T5  : three-way junctions, degree 3
#   C_*     : corner bends, degree 2 (no turn decision)

KIND_CORNER = 0    # degree-2 bend, no turn decision
KIND_THREE_WAY = 1  # T-junction
KIND_FOUR_WAY = 2  # crossroad


class Node:
    __slots__ = ("id", "gx", "gy", "kind", "label")

    def __init__(self, id, gx, gy, kind, label):
        self.id = id
        self.gx = gx
        self.gy = gy
        self.kind = kind
        self.label = label


# id, gx, gy, kind, label
NODES = [
    Node(0, 1.0, 1.0, KIND_FOUR_WAY, "X"),
    Node(1, 1.0, 0.0, KIND_THREE_WAY, "T1"),
    Node(2, 0.0, 1.0, KIND_THREE_WAY, "T3"),
    Node(3, 2.0, 1.0, KIND_THREE_WAY, "T4"),
    Node(4, 1.0, 2.0, KIND_THREE_WAY, "T5"),
    Node(5, 0.0, 0.0, KIND_CORNER, ""),
    Node(6, 2.0, 0.0, KIND_CORNER, ""),
    Node(7, 2.0, 2.0, KIND_CORNER, ""),
    Node(8, 0.0, 2.0, KIND_CORNER, ""),
]

# 12 edges: 8 perimeter ring + 4 spokes into the central crossroad. Orthogonal.
EDGES = [
    (5, 1), (1, 6), (6, 3), (3, 7), (7, 4), (4, 8), (8, 2), (2, 5),
    (1, 0), (4, 0), (2, 0), (3, 0),
]

_NODE_BY_ID = {n.id: n for n in NODES}


def node_by_id(nid):
    return _NODE_BY_ID.get(nid, NODES[0])


# Precompute neighbours (the graph is fixed).
_NEIGHBORS = {n.id: [] for n in NODES}
for _a, _b in EDGES:
    _NEIGHBORS[_a].append(_b)
    _NEIGHBORS[_b].append(_a)


def neighbors_of(nid):
    return _NEIGHBORS.get(nid, [])


# ============================================================================
# world - shared, mutable simulation state (world.cpp / world.h)
# ============================================================================
#
# REFERENCE NOTE: there are NO locks here, on purpose. Every Flow_TrafficLight,
# every Flow_Vehicle, and Flow_Visualization are modules on the SAME Runtime, so
# they advance on the one pump thread round-robin. A writer and a reader never
# overlap - the core uniflow guarantee. The UI thread only sees a copied Snapshot.

AXIS_NS = 0  # north-south (vertical roads)
AXIS_EW = 1  # east-west (horizontal roads)

MOVE_STRAIGHT = 0
MOVE_LEFT = 1

LIGHT_RED = 0
LIGHT_YELLOW = 1
LIGHT_GREEN = 2


class SignalState:
    """Live state of one intersection's signal. present=False for corners."""
    __slots__ = ("axis", "phase", "yellow", "present")

    def __init__(self, axis=AXIS_NS, phase=MOVE_STRAIGHT, yellow=False, present=False):
        self.axis = axis
        self.phase = phase
        self.yellow = yellow
        self.present = present


class VehicleState:
    """Every vehicle publishes its pose here each tick so peers can see each
    other (car-ahead spacing) and the renderer can draw them."""
    __slots__ = ("from_", "to", "dist", "gx", "gy", "dx", "dy", "blink",
                 "active", "r", "g", "b")

    def __init__(self):
        self.from_ = -1
        self.to = -1
        self.dist = 0.0
        self.gx = 0.0
        self.gy = 0.0
        self.dx = 1.0
        self.dy = 0.0
        self.blink = 0       # turn signal: -1 left, 0 none, +1 right
        self.active = False
        self.r = 200
        self.g = 200
        self.b = 200


# Shared signal table indexed by node id, and the vehicle table. Pump-only.
_SIGNALS = [SignalState() for _ in NODES]
_VEHICLES = []


def axis_of_edge(a, b):
    # Roads are orthogonal: equal grid-y means a horizontal (east-west) road.
    return AXIS_EW if a.gy == b.gy else AXIS_NS


def may_proceed(s, approach, turn_dir, axis_single_ended):
    if not s.present:
        return True              # corner bend - no signal
    if s.yellow:
        return False             # no new entries on yellow
    if approach != s.axis:
        return False             # cross axis is stopped
    if turn_dir < 0:
        return s.phase == MOVE_LEFT            # protected left
    if turn_dir > 0:
        return s.phase == MOVE_STRAIGHT or axis_single_ended
    return s.phase == MOVE_STRAIGHT            # straight


def axis_single_ended(node_id, axis):
    n = node_by_id(node_id)
    count = 0
    for nb in neighbors_of(node_id):
        if axis_of_edge(n, node_by_id(nb)) == axis:
            count += 1
    return count == 1


def straight_light(s, approach):
    if not s.present:
        return LIGHT_GREEN
    if approach != s.axis or s.phase != MOVE_STRAIGHT:
        return LIGHT_RED
    return LIGHT_YELLOW if s.yellow else LIGHT_GREEN


def set_signal(node_id, s):
    if 0 <= node_id < len(_SIGNALS):
        _SIGNALS[node_id] = s


def get_signal(node_id):
    if 0 <= node_id < len(_SIGNALS):
        return _SIGNALS[node_id]
    return SignalState()


def init_vehicles(n):
    global _VEHICLES
    _VEHICLES = [VehicleState() for _ in range(n)]


def set_vehicle(vid, s):
    if 0 <= vid < len(_VEHICLES):
        _VEHICLES[vid] = s


def vehicles():
    return _VEHICLES


# ============================================================================
# snapshot - pump -> UI hand-off (snapshot.cpp / snapshot.h)
# ============================================================================
#
# Flow_Visualization writes g_snap under g_snap_mu each tick; the render thread
# reads via read_snapshot(). This lock is the ONLY cross-thread synchronisation.

class VehicleView:
    __slots__ = ("gx", "gy", "dx", "dy", "blink", "r", "g", "b")

    def __init__(self, gx, gy, dx, dy, blink, r, g, b):
        self.gx = gx
        self.gy = gy
        self.dx = dx
        self.dy = dy
        self.blink = blink
        self.r = r
        self.g = g
        self.b = b


class Snapshot:
    __slots__ = ("signals", "vehicles")

    def __init__(self):
        self.signals = []      # SignalState per node id
        self.vehicles = []     # VehicleView


g_snap = Snapshot()
g_snap_mu = threading.Lock()


def read_snapshot():
    with g_snap_mu:
        s = Snapshot()
        s.signals = list(g_snap.signals)
        s.vehicles = list(g_snap.vehicles)
        return s


# ============================================================================
# Flow_TrafficLight - one autonomous signal per intersection (uf_traffic_light.*)
# ============================================================================
#
# Two-phase cycle (no left turns in this build): each phase gives one axis the
# green for green_, then yellow for yellow_, then hands the green to the other
# axis. NS green -> NS yellow -> EW green -> EW yellow -> (loop). One flow =
# one signal = one perpetual task.

class Flow_TrafficLight(uniflow.Uniflow):
    def __init__(self, rt, node_id):
        super().__init__(rt, name=node_by_id(node_id).label or "T?")
        self.ctx_signal = self.Task_Signal()
        self.AddTask(self.ctx_signal)
        self.ctx_signal.init(node_id)

    class Task_Signal(uniflow.Task):
        def init(self, node_id):
            self.node_id = node_id
            self.start_phase = node_id % 2
            # id-derived spreads so no two intersections share a period; long
            # hold so several cars clear per green. (ms in C++ -> seconds here)
            self.green = ((3500 + (node_id * 523) % 2100) * 3 // 2) / 1000.0
            self.yellow = (900 + (node_id * 311) % 500) / 1000.0
            self.timer = uniflow.UFTimer()

        def Entry(self):
            return self.Step_Begin()

        def publish(self, axis, yellow):
            s = SignalState(axis=axis, phase=MOVE_STRAIGHT,
                            yellow=yellow, present=True)
            set_signal(self.node_id, s)

        def run_phase(self, axis, label):
            yellow = self.timer.Passed(self.green)
            self.publish(axis, yellow)
            self.Describe(label, " (yellow)" if yellow else "")
            return self.timer.Passed(self.green + self.yellow)

        def Step_Begin(self):
            self.timer.Restart()
            if self.start_phase == 0:
                return self.Next(self.Step_NsGo)
            return self.Next(self.Step_EwGo)

        def Step_NsGo(self):
            if g_stop.is_set():
                return self.Done()
            if self.run_phase(AXIS_NS, "NS"):
                self.timer.Restart()
                return self.Next(self.Step_EwGo)
            return self.Stay()

        def Step_EwGo(self):
            if g_stop.is_set():
                return self.Done()
            if self.run_phase(AXIS_EW, "EW"):
                self.timer.Restart()
                return self.Next(self.Step_NsGo)
            return self.Stay()


# ============================================================================
# Flow_Vehicle - one car as a uniflow module (uf_vehicle.*)
# ============================================================================
#
# The driving loop is a single perpetual task whose steps form the FSM:
#   Step_Cruise - accelerate; brake to a stop at the stop line if not green
#   Step_Wait   - held at the stop line until the signal turns green
#   Step_Cross  - drive through the junction; pick the next edge (turn) and loop
#   Step_Turn   - arc through a junction/corner, then re-enter Cruise
# One flow = one car = one task that OWNS all the kinematic state.

K_CRUISE = 0.55      # grid units / second (target cruise speed)
K_TURN_SPEED = 0.26  # capped speed while turning through a node
K_ACCEL = 0.70       # grid units / second^2 (accel and braking)
K_SAFE_GAP = 0.15    # min centre-to-centre distance to a leader
K_CORNER_CUT = 0.12  # turn arc starts/ends this far before/after the node
K_ENTER_CLEAR = 0.27  # may enter a junction only if the exit edge is this clear
K_JUNCTION_ZONE = 0.25  # a car this far past our node is still "in the junction"


class Flow_Vehicle(uniflow.Uniflow):
    def __init__(self, rt, vid, frm, to, dist0, r, g, b):
        super().__init__(rt, name="Flow_Vehicle")
        self.ctx_drive = self.Task_Drive()
        self.AddTask(self.ctx_drive)
        self.ctx_drive.init(vid, frm, to, dist0, r, g, b)

    class Task_Drive(uniflow.Task):
        def init(self, vid, frm, to, dist0, r, g, b):
            self.id = vid
            self.from_ = frm
            self.to = to
            self.next = -1          # committed turn while crossing
            self.blink = 0          # turn signal: -1 left, +1 right
            self.dist = dist0       # [0,1] along edge from_ -> to
            self.speed = 0.0        # grid units per second
            self.turn_s = 0.0       # arc parameter [0,1] while turning
            self.ta = -1            # arc nodes captured at turn start
            self.tnode = -1
            self.tc = -1
            self.px = 0.0           # published centreline position
            self.py = 0.0
            self.hx = 1.0           # heading (normalised grid)
            self.hy = 0.0
            self.r = r
            self.g = g
            self.b = b
            self.last = time.perf_counter()
            self.rng = random.Random((vid * 2654435761 + 12345) & 0xFFFFFFFF)
            self.update_heading()

        def Entry(self):
            return self.Step_Cruise()

        # ----- kinematics helpers -----
        def dt(self):
            now = time.perf_counter()
            d = now - self.last
            self.last = now
            if d < 0.0:
                d = 0.0
            if d > 0.1:   # clamp the first/large gap so position never jumps
                d = 0.1
            return d

        def approach_speed(self, target, dt):
            step = K_ACCEL * dt
            if self.speed < target:
                self.speed = min(target, self.speed + step)
            else:
                self.speed = max(target, self.speed - step)
            if self.speed < 0.0:
                self.speed = 0.0

        def update_heading(self):
            a = node_by_id(self.from_)
            b = node_by_id(self.to)
            dx = b.gx - a.gx
            dy = b.gy - a.gy
            length = math.hypot(dx, dy)
            if length > 1e-9:
                self.hx = dx / length
                self.hy = dy / length

        def update_edge_pos(self):
            a = node_by_id(self.from_)
            b = node_by_id(self.to)
            self.px = a.gx + (b.gx - a.gx) * self.dist
            self.py = a.gy + (b.gy - a.gy) * self.dist
            self.update_heading()

        def publish(self):
            s = VehicleState()
            s.from_ = self.from_
            s.to = self.to
            s.dist = self.dist
            s.gx = self.px
            s.gy = self.py
            s.dx = self.hx
            s.dy = self.hy
            s.blink = self.blink
            s.active = True
            s.r = self.r
            s.g = self.g
            s.b = self.b
            set_vehicle(self.id, s)

        def gap_ahead(self, nxt):
            vs = vehicles()
            best = 1e9
            for i, o in enumerate(vs):
                if i == self.id or not o.active:
                    continue
                if o.from_ == self.from_ and o.to == self.to:
                    if o.dist > self.dist:
                        best = min(best, o.dist - self.dist)
                elif o.from_ == self.to:
                    # A car that has crossed our node: our path leader on the
                    # next edge, or any car still inside the junction zone.
                    my_path = (nxt >= 0 and o.to == nxt)
                    if my_path or o.dist < K_JUNCTION_ZONE:
                        best = min(best, (1.0 - self.dist) + o.dist)
            return best

        def next_edge_room(self, nxt):
            vs = vehicles()
            best = 1e9
            for i, o in enumerate(vs):
                if i == self.id:
                    continue
                if o.active and o.from_ == self.to and o.to == nxt:
                    best = min(best, o.dist)
            return best

        def turn_dir(self, n):
            nt = node_by_id(self.to)
            nn = node_by_id(n)
            ox = nn.gx - nt.gx
            oy = nn.gy - nt.gy
            length = math.hypot(ox, oy)
            if length > 1e-9:
                ox /= length
                oy /= length
            dot = self.hx * ox + self.hy * oy
            if dot > 0.8:
                return 0                        # straight
            cz = self.hx * oy - self.hy * ox    # y-down screen
            return 1 if cz > 0 else -1          # +1 right, -1 left

        def pick_next_node(self):
            prev = self.from_
            at = self.to
            pa = node_by_id(prev)
            pat = node_by_id(at)

            inx = pat.gx - pa.gx
            iny = pat.gy - pa.gy
            li = math.hypot(inx, iny)
            if li > 1e-9:
                inx /= li
                iny /= li

            cand = []
            cand_turn = []   # -1 left, 0 straight, +1 right
            for nb in neighbors_of(at):
                if nb == prev:
                    continue            # no U-turn
                n = node_by_id(nb)
                ox = n.gx - pat.gx
                oy = n.gy - pat.gy
                lo = math.hypot(ox, oy)
                if lo > 1e-9:
                    ox /= lo
                    oy /= lo
                dot = inx * ox + iny * oy
                if dot < -0.5:
                    continue            # would be a U-turn
                cz = inx * oy - iny * ox
                cand.append(nb)
                cand_turn.append(0 if dot > 0.8 else (1 if cz > 0 else -1))

            if not cand:
                return prev   # dead-end fallback (should not happen on this map)

            # No left turns in this build: drop left exits when a straight/right
            # option exists (keep them only as a last resort).
            kc = [cand[k] for k in range(len(cand)) if cand_turn[k] != -1]
            kt = [cand_turn[k] for k in range(len(cand)) if cand_turn[k] != -1]
            if kc:
                cand = kc
                cand_turn = kt

            # 60%: follow the car ahead on this edge, matching its turn, so cars
            # form platoons and the map looks busier.
            has_leader = False
            leader_turn = 0
            best_dist = 1e9
            for i, o in enumerate(vehicles()):
                if i == self.id:
                    continue
                if (o.active and o.from_ == self.from_ and o.to == self.to and
                        o.dist > self.dist and o.dist < best_dist):
                    best_dist = o.dist
                    leader_turn = o.blink
                    has_leader = True
            if has_leader and self.rng.random() < 0.6:
                for k in range(len(cand)):
                    if cand_turn[k] == leader_turn:
                        return cand[k]

            return cand[self.rng.randrange(len(cand))]

        # ----- the driving state machine -----
        def Step_Cruise(self):
            if g_stop.is_set():
                return self.Done()

            # Decide the route for this edge up front so we know which signal
            # phase to obey and can signal the turn on the approach.
            if self.next < 0:
                self.next = self.pick_next_node()

            dt = self.dt()
            a = node_by_id(self.from_)
            b = node_by_id(self.to)
            ax = axis_of_edge(a, b)
            td = self.turn_dir(self.next)
            junction = b.kind != KIND_CORNER
            self.blink = td if junction else 0

            sig = get_signal(self.to)
            may_go = may_proceed(sig, ax, td, axis_single_ended(self.to, ax))
            exit_blocked = self.next_edge_room(self.next) < K_ENTER_CLEAR

            stopline = 1.0 - GEO_STOP_OFFSET
            need_stop = (not may_go) or exit_blocked

            # Nearest point we must not pass: stop line (if stopping) or the safe
            # distance behind the leader on this edge - whichever is closer.
            bind = 1e9
            if need_stop:
                bind = min(bind, stopline)
            lead_gap = self.gap_ahead(-1)
            if lead_gap < 1e8:
                bind = min(bind, self.dist + (lead_gap - K_SAFE_GAP))

            clearance = bind - self.dist
            target = 0.0 if clearance <= 0.0 else min(
                K_CRUISE, math.sqrt(2.0 * K_ACCEL * clearance))

            prev = self.dist
            self.approach_speed(target, dt)
            self.dist += self.speed * dt
            if self.dist > bind:          # do not overshoot the constraint
                self.dist = max(prev, bind)
                self.speed = 0.0
            self.update_edge_pos()
            self.publish()

            if self.dist >= stopline - 1e-9:
                if may_go and not exit_blocked:
                    return self.Next(self.Step_Cross)
                self.speed = 0.0
                return self.Next(self.Step_Wait)

            self.Describe("cruise")
            return self.Stay()

        def Step_Wait(self):
            if g_stop.is_set():
                return self.Done()

            self.dt()   # keep the clock fresh so Cross starts with a small dt
            self.speed = 0.0
            self.update_edge_pos()
            self.publish()

            if self.next < 0:
                self.next = self.pick_next_node()

            a = node_by_id(self.from_)
            b = node_by_id(self.to)
            ax = axis_of_edge(a, b)
            td = self.turn_dir(self.next)
            junction = b.kind != KIND_CORNER
            self.blink = td if junction else 0

            sig = get_signal(self.to)
            if (may_proceed(sig, ax, td, axis_single_ended(self.to, ax)) and
                    self.next_edge_room(self.next) >= K_ENTER_CLEAR):
                return self.Next(self.Step_Cross)

            self.Describe("wait")
            return self.Stay()

        def Step_Cross(self):
            if g_stop.is_set():
                return self.Done()

            if self.next < 0:
                self.next = self.pick_next_node()

            dt = self.dt()

            # Is this move a turn? Compare incoming heading with the outgoing one.
            nt = node_by_id(self.to)
            nn = node_by_id(self.next)
            ox = nn.gx - nt.gx
            oy = nn.gy - nt.gy
            lo = math.hypot(ox, oy)
            if lo > 1e-9:
                ox /= lo
                oy /= lo
            turn_dot = self.hx * ox + self.hy * oy  # 1 straight, 0 = 90 deg turn
            turning = turn_dot < 0.8
            # A turn pivots early: hand off to the arc K_CORNER_CUT before node.
            handoff = (1.0 - K_CORNER_CUT) if turning else 1.0

            cz = self.hx * oy - self.hy * ox        # turn direction (y-down)
            at_junction = node_by_id(self.to).kind != KIND_CORNER
            self.blink = (1 if cz > 0 else -1) if (turning and at_junction) else 0

            lead_gap = self.gap_ahead(self.next)
            bind = self.dist + (lead_gap - K_SAFE_GAP) if lead_gap < 1e8 else 1e9
            clearance = bind - self.dist
            target = 0.0 if clearance <= 0.0 else min(
                K_CRUISE, math.sqrt(2.0 * K_ACCEL * clearance))
            if turning:
                target = min(target, K_TURN_SPEED)

            prev = self.dist
            self.approach_speed(target, dt)
            self.dist += self.speed * dt
            if self.dist > bind:
                self.dist = max(prev, bind)
                self.speed = 0.0
            if self.dist > handoff:
                self.dist = handoff   # never roll past the arc's start
            self.update_edge_pos()
            self.publish()

            if turning and self.dist >= handoff - 1e-9:
                self.turn_s = 0.0
                self.ta = self.from_     # capture arc nodes before mutating
                self.tnode = self.to
                self.tc = self.next
                return self.Next(self.Step_Turn)
            if (not turning) and self.dist >= 1.0 - 1e-9:
                self.from_ = self.to
                self.to = self.next
                self.dist -= 1.0
                if self.dist < 0.0:
                    self.dist = 0.0
                self.next = -1
                self.update_heading()
                return self.Next(self.Step_Cruise)

            self.Describe("cross")
            return self.Stay()

        def Step_Turn(self):
            if g_stop.is_set():
                return self.Done()

            dt = self.dt()
            self.approach_speed(K_TURN_SPEED, dt)

            # Quadratic Bezier across the corner, using the nodes captured at
            # entry: from K_CORNER_CUT before the node, through it, to
            # K_CORNER_CUT along the next edge.
            a = node_by_id(self.ta)      # edge start
            nd = node_by_id(self.tnode)  # the node (corner)
            c = node_by_id(self.tc)      # next edge end

            inx = nd.gx - a.gx
            iny = nd.gy - a.gy
            li = math.hypot(inx, iny)
            if li > 1e-9:
                inx /= li
                iny /= li
            oux = c.gx - nd.gx
            ouy = c.gy - nd.gy
            lo = math.hypot(oux, ouy)
            if lo > 1e-9:
                oux /= lo
                ouy /= lo

            p0x = nd.gx - inx * K_CORNER_CUT
            p0y = nd.gy - iny * K_CORNER_CUT
            p1x = nd.gx
            p1y = nd.gy
            p2x = nd.gx + oux * K_CORNER_CUT
            p2y = nd.gy + ouy * K_CORNER_CUT

            # Advance the arc parameter by distance / (approx arc length).
            approx_len = 1.6 * K_CORNER_CUT
            self.turn_s += (self.speed * dt) / approx_len
            if self.turn_s > 1.0:
                self.turn_s = 1.0
            s = self.turn_s

            self.px = (1 - s) * (1 - s) * p0x + 2 * (1 - s) * s * p1x + s * s * p2x
            self.py = (1 - s) * (1 - s) * p0y + 2 * (1 - s) * s * p1y + s * s * p2y

            tx = 2 * (1 - s) * (p1x - p0x) + 2 * s * (p2x - p1x)
            ty = 2 * (1 - s) * (p1y - p0y) + 2 * s * (p2y - p1y)
            lt = math.hypot(tx, ty)
            if lt > 1e-9:
                self.hx = tx / lt
                self.hy = ty / lt

            # Occupy the destination edge so another car cannot turn into the
            # same spot and overlap us mid-corner.
            self.from_ = self.tnode
            self.to = self.tc
            self.dist = s * K_CORNER_CUT
            self.publish()

            if self.turn_s >= 1.0:
                self.dist = K_CORNER_CUT  # emerge just past the node
                self.next = -1
                self.blink = 0
                self.update_heading()
                return self.Next(self.Step_Cruise)

            self.Describe("turn")
            return self.Stay()


# ============================================================================
# Flow_Visualization - copies World -> Snapshot each tick (uf_visualization.*)
# ============================================================================
#
# The pump side (Step_Tick) snapshots the shared World (signals + vehicle poses)
# under g_snap_mu so the render thread sees a consistent frame. That lock is the
# ONLY cross-thread synchronisation in the demo.

class Flow_Visualization(uniflow.Uniflow):
    def __init__(self, rt):
        super().__init__(rt, name="Flow_Visualization")
        self.ctx_snapshot = self.Task_Snapshot()
        self.AddTask(self.ctx_snapshot)

    class Task_Snapshot(uniflow.Task):
        def Entry(self):
            return self.Step_Tick()

        def Step_Tick(self):
            if g_stop.is_set():
                return self.Done()

            sigs = [get_signal(n.id) for n in NODES]
            cars = []
            for v in vehicles():
                if not v.active:
                    continue
                cars.append(VehicleView(v.gx, v.gy, v.dx, v.dy, v.blink,
                                        v.r, v.g, v.b))

            with g_snap_mu:
                g_snap.signals = sigs
                g_snap.vehicles = cars
            return self.Stay()


# ============================================================================
# console renderer - ANSI raster back-end (uf_visualization_console.cpp)
# ============================================================================
#
# Rasterises the grid map, per-approach signal lamps, and the moving fleet onto
# a character canvas with 24-bit colour. Cols-per-grid-unit is ~2x rows so the
# square map looks square despite terminal cells being ~2:1 tall.

K_SX = 24                # columns per grid unit
K_SY = 10                # rows per grid unit
K_COLS = 2 * K_SX + 1    # grid spans [0,2]
K_ROWS = 2 * K_SY + 1
K_HEADER_ROWS = 3        # title lines above the map


def _col(gx):
    return int(round(gx * K_SX))


def _row(gy):
    return int(round(gy * K_SY))


def _lamp_color(c):
    if c == LIGHT_GREEN:
        return console.fg(60, 220, 100)
    if c == LIGHT_YELLOW:
        return console.fg(240, 200, 60)
    return console.fg(170, 70, 70)   # dim red


class _Cell:
    __slots__ = ("ch", "color")

    def __init__(self):
        self.ch = " "
        self.color = ""          # ANSI SGR prefix; empty = default


def _put(canvas, r, c, ch, col):
    if r < 0 or r >= K_ROWS or c < 0 or c >= K_COLS:
        return
    cell = canvas[r * K_COLS + c]
    cell.ch = ch
    cell.color = col


def draw_console(s):
    canvas = [_Cell() for _ in range(K_ROWS * K_COLS)]
    road = console.GRAY

    # 1) roads: a dim line of '.' between each pair of connected nodes.
    for ea, eb in EDGES:
        a = node_by_id(ea)
        b = node_by_id(eb)
        ca, ra = _col(a.gx), _row(a.gy)
        cb, rb = _col(b.gx), _row(b.gy)
        if ra == rb:   # horizontal road
            for c in range(min(ca, cb), max(ca, cb) + 1):
                _put(canvas, ra, c, ".", road)
        else:          # vertical road
            for r in range(min(ra, rb), max(ra, rb) + 1):
                _put(canvas, r, ca, ".", road)

    # 2) signal lamps: one 'O' per approach, placed a few cells out from the
    #    junction toward the approaching road, lit by that approach's signal.
    for n in NODES:
        if n.kind == KIND_CORNER:
            continue
        if n.id >= len(s.signals):
            continue
        st = s.signals[n.id]
        nc, nr = _col(n.gx), _row(n.gy)
        for nb in neighbors_of(n.id):
            m = node_by_id(nb)
            dc = (1 if m.gx > n.gx else 0) - (1 if m.gx < n.gx else 0)
            dr = (1 if m.gy > n.gy else 0) - (1 if m.gy < n.gy else 0)
            ax = axis_of_edge(n, m)
            col = _lamp_color(straight_light(st, ax))
            _put(canvas, nr + dr * 2, nc + dc * 3, "O", col)

    # 3) nodes: junctions show their label initial (X / T), corners a '+'.
    node_col = console.BOLD + console.fg(225, 230, 240)
    for n in NODES:
        if n.kind == KIND_CORNER:
            ch = "+"
        else:
            ch = n.label[0] if n.label else "#"
        _put(canvas, _row(n.gy), _col(n.gx), ch, node_col)

    # 4) vehicles (drawn last, on top): an arrow in the car's own colour,
    #    pointing the way it is heading.
    for v in s.vehicles:
        if abs(v.dx) >= abs(v.dy):
            arrow = ">" if v.dx >= 0 else "<"
        else:
            arrow = "v" if v.dy >= 0 else "^"   # grid y grows downward
        _put(canvas, _row(v.gy), _col(v.gx), arrow, console.fg(v.r, v.g, v.b))

    # compose one frame and write it in a single flush to avoid flicker.
    out = []
    out.append(console.at(1, 1))
    out.append(console.CLEAR_LINE)
    out.append("  " + console.BOLD + "uniflow city traffic  " + console.RESET
               + console.DIM + "v" + uniflow.__version__ + console.RESET)
    out.append(console.at(2, 1) + console.CLEAR_LINE + "  " + console.GRAY
               + "one pump thread: signals, acceleration, car-ahead spacing, "
               + "intersection yielding" + console.RESET)
    out.append(console.at(3, 1) + console.CLEAR_LINE + "  " + console.DIM
               + "press Enter to quit" + console.RESET)

    for r in range(K_ROWS):
        out.append(console.at(r + 1 + K_HEADER_ROWS, 1) + console.CLEAR_LINE)
        cur = ""   # current active colour to minimise escape spam
        for c in range(K_COLS):
            cell = canvas[r * K_COLS + c]
            if cell.color != cur:
                out.append(console.RESET if cell.color == "" else cell.color)
                cur = cell.color
            out.append(cell.ch)
        out.append(console.RESET)

    sys.stdout.write("".join(out))
    sys.stdout.flush()


def run_visualisation_console():
    console.enable_ansi()
    console.hide_cursor()
    console.clear()

    # Render on a background thread; the main thread blocks on stdin so a single
    # Enter quits. The render thread only READS the snapshot (lock-guarded).
    done = threading.Event()

    def render_loop():
        while not done.is_set() and not g_stop.is_set():
            draw_console(read_snapshot())
            time.sleep(0.033)   # ~30 fps

    render = threading.Thread(target=render_loop, name="render", daemon=True)
    render.start()

    try:
        sys.stdin.readline()    # any Enter (or EOF) quits
    except (EOFError, KeyboardInterrupt):
        pass
    done.set()
    render.join(timeout=1.0)

    console.show_cursor()
    sys.stdout.write(console.at(K_ROWS + K_HEADER_ROWS + 2, 1)
                     + console.CLEAR_LINE + "  city_traffic stopped.\n")
    sys.stdout.flush()


# ============================================================================
# App - the Runtime plus every module (app.h). Two-phase init.
# ============================================================================

def _hsv(h, s, v):
    """HSV -> (r,g,b) 0..255, for spreading distinct car colours."""
    c = v * s
    x = c * (1.0 - abs((h / 60.0) % 2.0 - 1.0))
    m = v - c
    if h < 60:
        rr, gg, bb = c, x, 0
    elif h < 120:
        rr, gg, bb = x, c, 0
    elif h < 180:
        rr, gg, bb = 0, c, x
    elif h < 240:
        rr, gg, bb = 0, x, c
    elif h < 300:
        rr, gg, bb = x, 0, c
    else:
        rr, gg, bb = c, 0, x
    return (int((rr + m) * 255.0), int((gg + m) * 255.0), int((bb + m) * 255.0))


class App:
    def __init__(self):
        # Silent runtime: the ANSI renderer OWNS stdout, so the default
        # ConsoleObserver's per-step trace must be suppressed. Short all-Stay
        # naps keep motion and the renderer smooth (sleeps in SECONDS).
        cfg = uniflow.Config(idle_sleep=0.001,
                             stay_sleep=0.005,
                             step_interval_sleep=0.0)
        self.rt = uniflow.Runtime(observer=uniflow.Observer(), config=cfg)

        # Phase 1: construct. visualisation, then one light per junction, then
        # the fleet.
        self.viz = Flow_Visualization(self.rt)

        self.lights = []
        for n in NODES:
            if n.kind != KIND_CORNER:
                self.lights.append(Flow_TrafficLight(self.rt, n.id))

        self.vehicles = []
        self.build_fleet()

    def build_fleet(self):
        n = 15
        e_count = len(EDGES)
        init_vehicles(n)
        for i in range(n):
            ea, eb = EDGES[i % e_count]
            slot = i // e_count                 # 0 or 1 cars per edge
            dist0 = 0.22 + 0.45 * slot
            r, g, b = _hsv((i * 137.5) % 360.0, 0.62, 0.96)  # golden angle
            self.vehicles.append(
                Flow_Vehicle(self.rt, i, ea, eb, dist0, r, g, b))

    def Start(self):
        # Phase 2: launch every task. Each StartFlow() puts one task on the pump.
        self.viz.ctx_snapshot.StartFlow()
        for light in self.lights:
            light.ctx_signal.StartFlow()
        for v in self.vehicles:
            v.ctx_drive.StartFlow()

    def Shutdown(self):
        g_stop.set()        # every step checks this and returns Done()
        self.rt.Wake()      # nudge the pump out of any sleep
        for v in self.vehicles:
            v.WaitUntilIdle()
        for light in self.lights:
            light.WaitUntilIdle()
        self.viz.WaitUntilIdle()
        self.rt.stop()


# ============================================================================
# main (main.cpp): construct modules, arm flows, render, shut down.
# ============================================================================

def main():
    app = App()        # phase 1: Runtime + modules constructed
    app.Start()        # phase 2: flows armed

    try:
        run_visualisation_console()   # main-thread render loop (console)
    finally:
        app.Shutdown()


if __name__ == "__main__":
    main()
