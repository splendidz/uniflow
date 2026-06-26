"""pick_and_place.py - one-pump-thread CNC pick-and-place line (CONSOLE renderer).

Python port of cpp/examples/pick_and_place/ (console back-end only; the Win32
GDI window is dropped - Python has no Win32 here). A pick-and-place line runs on
ONE Runtime pump thread:

  - Flow_LoadPicker  moves a raw part  zone A -> zone B  (Pick then Place tasks)
  - Flow_Stage       machines it at B  (Prepare -> Process -> Cleanup tasks)
  - Flow_UnloadPicker moves the finished part  zone B -> zone C  (Pick then Place)
  - Flow_Orchestrator schedules every module's next task by state; the pickers
    and the stage never sequence themselves.

FEATURE FOCUS:
  - orchestrator + state polling: one perpetual Schedule task launches each
    module's next task when that module IsIdle, chosen from plain member reads.
  - lock-free B-zone handoff on ONE pump: the two pickers must NEVER both sit in
    zone B. Solved purely by reading peer members (InsideZoneB / Carrying); no
    locks, because every module advances on the same pump thread round-robin.
  - async-poll command acks: the Stage's start/cleanup commands are SubmitAsync
    workers, polled by AsyncId with a StayTimeout timeout catch - the pump never
    blocks on them.
  - multi-task module: one Uniflow module (the Stage) holds three tasks and the
    orchestrator runs them one at a time.

NOTE ON UNITS: the C++ port measures durations in milliseconds; the Python
VirtualClock / UFTimer report SECONDS, so the same waits are written in seconds
(process 5.0 s, ack timeout 2.0 s, hw settle 0.05 s, etc).

NOTE ON MOTION: the C++ port integrates its MotorAxis objects on a side thread
(MotorIOFactory). Here the axes integrate per pump tick (dt since last poll)
inside a tiny MotorAxis helper - the simplest faithful approach on one thread.
"""

import os
import sys
import random
import threading
import time

# --- import shim: make python/ importable, then the sibling console helper ---
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import uniflow  # noqa: E402
import console  # noqa: E402  (same dir as this file)


# ============================================================================
# globals - dimensional constants, timing, line-level state (mirrors globals.*)
# ============================================================================

class Geometry:
    ZONE_A_MM = 200.0
    ZONE_B_MM = 700.0
    ZONE_C_MM = 1200.0
    X_MAX_MM = 1400.0
    B_SAFETY_GAP_MM = 250.0
    STAGE_TRAVEL_MM = 80.0

    Z_UP_MM = 0.0
    Z_DOWN_MM = 120.0          # down is positive

    X_SPEED_MM_PER_S = 300.0
    Z_SPEED_MM_PER_S = 200.0

    FINGER_OPEN_MM = 24.0
    PART_WIDTH_MM = 20.0
    FINGER_SPEED_MM_PER_S = 200.0

    @staticmethod
    def inside_zone_b(x_mm):
        return abs(x_mm - Geometry.ZONE_B_MM) < Geometry.B_SAFETY_GAP_MM


# The machining phases, made explicit so the orchestrator can launch the
# Stage's tasks one at a time: RawPartLoaded -> (Prepare) -> Prepared ->
# (Process) -> Machined -> (Cleanup) -> ProcessedPartReady.
class StageState:
    IDLE = "Idle"
    RAW_PART_LOADED = "RawPartLoaded"
    PREPARED = "Prepared"
    MACHINED = "Machined"
    PROCESSED_PART_READY = "ProcessedPartReady"


# Line-level environment. Plain pump-thread state (the pickers/stage/orchestrator
# all run on the one pump), except Stop which the main/stdin thread sets.
class Env:
    _zone_a_part = False
    _delivered = 0
    _stop = False

    @classmethod
    def zone_a_has_part(cls):
        return cls._zone_a_part

    @classmethod
    def create_fake_zone_a_part(cls):
        cls._zone_a_part = True

    @classmethod
    def consume_zone_a_part(cls):
        cls._zone_a_part = False

    @classmethod
    def delivered_count(cls):
        return cls._delivered

    @classmethod
    def inc_delivered(cls):
        cls._delivered += 1

    @classmethod
    def stop(cls):
        return cls._stop

    @classmethod
    def request_stop(cls):
        cls._stop = True


# ============================================================================
# motor_io - MotorAxis / DigitalLatch integrated per pump tick (mirrors
# motor_io_factory.*). The C++ port integrates these on a side thread; here a
# step polls Update(dt) once per round, so position advances on the one pump
# thread. No locks needed - everything touches them on the pump thread.
# ============================================================================

class MotorAxis:
    """1D linear axis that integrates its own position toward a commanded target
    at a fixed speed. A step only commands (move/home) and polls (in_position).
    update(dt_s) is driven once per pump round by the owning module."""

    def __init__(self, name, initial_mm, speed_mm_per_s):
        self.name = name
        self._pos = initial_mm
        self._target = initial_mm
        self._speed = speed_mm_per_s
        self._home = initial_mm
        self._moving = False

    def move(self, target_mm):
        self._target = target_mm
        self._moving = abs(self._pos - target_mm) > 1e-6

    def home(self):
        self.move(self._home)

    def position(self):
        return self._pos

    def in_position(self):
        return not self._moving

    def update(self, dt_s):
        if not self._moving:
            return
        remaining = self._target - self._pos
        step = self._speed * dt_s
        if abs(remaining) <= step:
            self._pos = self._target
            self._moving = False
        else:
            self._pos += step if remaining > 0.0 else -step


class DigitalLatch:
    """A digital input that latches true a random delay after Arm() - the demo's
    model for a hardware handshake. The delay is counted down by update(dt)."""

    def __init__(self, name, min_delay_s, max_delay_s):
        self.name = name
        self._min = min_delay_s
        self._max = max_delay_s
        self._remaining = 0.0
        self._armed = False
        self._ready = False

    def arm(self):
        self._remaining = random.uniform(self._min, self._max)
        self._armed = True
        self._ready = False

    def reset(self):
        self._armed = False
        self._ready = False
        self._remaining = 0.0

    def is_ready(self):
        return self._ready

    def update(self, dt_s):
        if not self._armed:
            return
        self._remaining -= dt_s
        if self._remaining <= 0.0:
            self._ready = True
            self._armed = False


class _DeviceClock:
    """Per-module helper: tracks dt between pump rounds and ticks a device list.
    Each module calls tick() once at the top of every step so its borrowed axes
    advance with real elapsed time. Mirrors the C++ factory thread's 4ms loop,
    but folded onto the pump thread."""

    def __init__(self):
        self._devices = []
        self._last = None

    def add(self, device):
        self._devices.append(device)
        return device

    def tick(self):
        now = time.monotonic()
        if self._last is None:
            self._last = now
            return
        dt = now - self._last
        self._last = now
        for d in self._devices:
            d.update(dt)


# ============================================================================
# snapshot - pump -> render hand-off (mirrors snapshot.*). The render thread
# reads it under g_snap_mu; the only cross-thread lock in the demo.
# ============================================================================

class Snapshot:
    def __init__(self):
        self.load_x_mm = Geometry.ZONE_A_MM
        self.load_z_mm = Geometry.Z_UP_MM
        self.load_carry = False
        self.load_phase = "-"

        self.unload_x_mm = Geometry.ZONE_C_MM
        self.unload_z_mm = Geometry.Z_UP_MM
        self.unload_carry = False
        self.unload_phase = "-"

        self.stage_table_x_mm = Geometry.ZONE_B_MM
        self.stage_table_y_mm = 0.0
        self.stage_state = StageState.IDLE
        self.stage_phase = "-"

        self.zone_a_has_part = False
        self.delivered = 0


g_snap = Snapshot()
g_snap_mu = threading.Lock()


def read_snapshot():
    with g_snap_mu:
        import copy
        return copy.copy(g_snap)


# ============================================================================
# Flow_LoadPicker - carries a raw part A -> B. Two tasks: Pick (A) -> Place (B).
# ============================================================================

class Flow_LoadPicker(uniflow.Uniflow):
    def __init__(self, rt):
        super().__init__(rt, name="Flow_LoadPicker")
        self._dev = _DeviceClock()
        self.x = self._dev.add(MotorAxis("load_x", Geometry.ZONE_A_MM,
                                         Geometry.X_SPEED_MM_PER_S))
        self.z = self._dev.add(MotorAxis("load_z", Geometry.Z_UP_MM,
                                         Geometry.Z_SPEED_MM_PER_S))
        self.finger = self._dev.add(MotorAxis("load_finger", Geometry.FINGER_OPEN_MM,
                                              Geometry.FINGER_SPEED_MM_PER_S))
        self.carrying = False

        self.ctx_pick = self.Task_Pick()
        self.AddTask(self.ctx_pick)
        self.ctx_place = self.Task_Place()
        self.AddTask(self.ctx_place)

    # -- Motion state read by the unload picker's B-zone gate and the snapshot --
    def x_mm(self):
        return self.x.position()

    def z_mm(self):
        return self.z.position()

    def carrying_flag(self):
        return self.carrying

    def inside_zone_b(self):
        return Geometry.inside_zone_b(self.x_mm())

    def partner_in_zone_b(self):
        return App.inst().unload.inside_zone_b()

    # ----- Task: Pick (zone A) -----
    class Task_Pick(uniflow.Task):
        def Entry(self):
            return self.Step1_CmdMoveToSource()

        def Step1_CmdMoveToSource(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            self.Describe("cmd: move to zone A")
            f.x.move(Geometry.ZONE_A_MM)
            return self.Next(self.Step2_WaitAtSource)

        def Step2_WaitAtSource(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            self.Describe("approaching A")
            if f.x.in_position():
                return self.Next(self.Step3_CmdLowerToPick)
            return self.Stay()

        def Step3_CmdLowerToPick(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            self.Describe("cmd: lower to pick")
            f.z.move(Geometry.Z_DOWN_MM)
            return self.Next(self.Step4_WaitAtPickDown)

        def Step4_WaitAtPickDown(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            self.Describe("lowering to pick")
            if f.z.in_position():
                return self.Next(self.Step5_HandGrip)
            return self.Stay()

        def Step5_HandGrip(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            self.Describe("closing gripper")
            f.finger.move(0.0)
            if not f.finger.in_position():
                return self.Stay()
            Env.consume_zone_a_part()
            f.carrying = True
            return self.Next(self.Step6_CmdLiftWithPart)

        def Step6_CmdLiftWithPart(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            self.Describe("cmd: lift with part")
            f.z.move(Geometry.Z_UP_MM)
            return self.Next(self.Step7_WaitAtPickUp)

        def Step7_WaitAtPickUp(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            self.Describe("lifting with part")
            # Pick done: part is up and carried. The flow goes idle; the
            # orchestrator launches Place next (it sees carrying()).
            if f.z.in_position():
                return self.Done()
            return self.Stay()

    # ----- Task: Place (zone B) - gated by stage readiness + partner -----
    class Task_Place(uniflow.Task):
        def Entry(self):
            return self.Step1_CmdMoveToDest()

        def Step1_CmdMoveToDest(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            stage = App.inst().stage
            # lock-free B-zone handoff: read the stage's readiness and the
            # partner's position - plain member reads on the one pump thread.
            may_enter_b = (stage.ready_to_receive_raw_part()
                           and not f.partner_in_zone_b())
            if not f.inside_zone_b() and not may_enter_b:
                f.x.move(Geometry.ZONE_B_MM - Geometry.B_SAFETY_GAP_MM)
                if not f.x.in_position():
                    self.Describe("moving to A-gap")
                    return self.Stay()
                self.Describe("parked at A-gap: stage=", stage.state(),
                              " partner_in_B=", f.partner_in_zone_b())
                return self.Stay()
            self.Describe("cmd: move to zone B")
            f.x.move(Geometry.ZONE_B_MM)
            return self.Next(self.Step2_WaitAtDest)

        def Step2_WaitAtDest(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            self.Describe("approaching B")
            if f.x.in_position():
                return self.Next(self.Step3_CmdLowerToPlace)
            return self.Stay()

        def Step3_CmdLowerToPlace(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            self.Describe("cmd: lower to place")
            f.z.move(Geometry.Z_DOWN_MM)
            return self.Next(self.Step4_WaitAtPlaceDown)

        def Step4_WaitAtPlaceDown(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            self.Describe("lowering to place")
            if f.z.in_position():
                return self.Next(self.Step5_HandRelease)
            return self.Stay()

        def Step5_HandRelease(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            self.Describe("opening gripper")
            f.finger.move(Geometry.FINGER_OPEN_MM)
            if not f.finger.in_position():
                return self.Stay()
            App.inst().stage.on_raw_part_received()
            f.carrying = False
            return self.Next(self.Step6_CmdLiftEmpty)

        def Step6_CmdLiftEmpty(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            self.Describe("cmd: lift empty")
            f.z.move(Geometry.Z_UP_MM)
            return self.Next(self.Step7_WaitAtPlaceUp)

        def Step7_WaitAtPlaceUp(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            self.Describe("lifting empty")
            if f.z.in_position():
                return self.Next(self.Step8_CmdRetreat)
            return self.Stay()

        def Step8_CmdRetreat(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            self.Describe("cmd: retreat to A")
            f.x.move(Geometry.ZONE_A_MM)
            return self.Next(self.Step9_WaitAtRetreat)

        def Step9_WaitAtRetreat(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            self.Describe("retreating")
            if f.x.in_position():
                self.Describe("flow done")
                return self.Done()
            return self.Stay()


# ============================================================================
# Flow_UnloadPicker - carries the finished part B -> C. Same shape, but the
# SOURCE is the contested B zone (Pick at B -> Place at C).
# ============================================================================

class Flow_UnloadPicker(uniflow.Uniflow):
    def __init__(self, rt):
        super().__init__(rt, name="Flow_UnloadPicker")
        self._dev = _DeviceClock()
        self.x = self._dev.add(MotorAxis("unload_x", Geometry.ZONE_C_MM,
                                         Geometry.X_SPEED_MM_PER_S))
        self.z = self._dev.add(MotorAxis("unload_z", Geometry.Z_UP_MM,
                                         Geometry.Z_SPEED_MM_PER_S))
        self.finger = self._dev.add(MotorAxis("unload_finger", Geometry.FINGER_OPEN_MM,
                                              Geometry.FINGER_SPEED_MM_PER_S))
        self.carrying = False

        self.ctx_pick = self.Task_Pick()
        self.AddTask(self.ctx_pick)
        self.ctx_place = self.Task_Place()
        self.AddTask(self.ctx_place)

    def x_mm(self):
        return self.x.position()

    def z_mm(self):
        return self.z.position()

    def carrying_flag(self):
        return self.carrying

    def inside_zone_b(self):
        return Geometry.inside_zone_b(self.x_mm())

    def partner_in_zone_b(self):
        return App.inst().load.inside_zone_b()

    # ----- Task: Pick (zone B, contested) -----
    class Task_Pick(uniflow.Task):
        def Entry(self):
            return self.Step1_CmdMoveToSource()

        def Step1_CmdMoveToSource(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            load = App.inst().load
            st = App.inst().stage.state()
            stage_past_loading = st in (StageState.PREPARED, StageState.MACHINED,
                                        StageState.PROCESSED_PART_READY)
            # load.carrying catches "approaching B with a part"; inside_zone_b
            # catches "still lifting/retreating in B after the handoff".
            load_threatens_b = load.carrying_flag() or load.inside_zone_b()
            may_enter_b = stage_past_loading and not load_threatens_b
            if not f.inside_zone_b() and not may_enter_b:
                f.x.move(Geometry.ZONE_B_MM + Geometry.B_SAFETY_GAP_MM)
                if not f.x.in_position():
                    self.Describe("moving to C-gap")
                    return self.Stay()
                self.Describe("parked at C-gap: stage=", st,
                              " load_carry=", load.carrying_flag(),
                              " load_in_B=", load.inside_zone_b())
                return self.Stay()
            self.Describe("cmd: move to zone B")
            f.x.move(Geometry.ZONE_B_MM)
            return self.Next(self.Step2_WaitAtSource)

        def Step2_WaitAtSource(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            self.Describe("approaching B")
            if f.x.in_position():
                return self.Next(self.Step3_CmdLowerToPick)
            return self.Stay()

        def Step3_CmdLowerToPick(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            if not App.inst().stage.ready_to_hand_off_processed_part():
                self.Describe("hovering above B: stage=", App.inst().stage.state())
                return self.Stay()
            self.Describe("cmd: lower to pick")
            f.z.move(Geometry.Z_DOWN_MM)
            return self.Next(self.Step4_WaitAtPickDown)

        def Step4_WaitAtPickDown(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            self.Describe("lowering to pick")
            if f.z.in_position():
                return self.Next(self.Step5_HandGrip)
            return self.Stay()

        def Step5_HandGrip(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            self.Describe("closing gripper")
            f.finger.move(0.0)
            if not f.finger.in_position():
                return self.Stay()
            App.inst().stage.on_processed_part_taken()
            f.carrying = True
            return self.Next(self.Step6_CmdLiftWithPart)

        def Step6_CmdLiftWithPart(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            self.Describe("cmd: lift with part")
            f.z.move(Geometry.Z_UP_MM)
            return self.Next(self.Step7_WaitAtPickUp)

        def Step7_WaitAtPickUp(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            self.Describe("lifting with part")
            if f.z.in_position():
                return self.Done()
            return self.Stay()

    # ----- Task: Place (zone C) -----
    class Task_Place(uniflow.Task):
        def Entry(self):
            return self.Step1_CmdMoveToUnload()

        def Step1_CmdMoveToUnload(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            self.Describe("cmd: move to zone C")
            f.x.move(Geometry.ZONE_C_MM)
            return self.Next(self.Step2_WaitAtUnload)

        def Step2_WaitAtUnload(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            self.Describe("approaching C")
            if f.x.in_position():
                return self.Next(self.Step3_CmdLowerToPlace)
            return self.Stay()

        def Step3_CmdLowerToPlace(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            self.Describe("cmd: lower to place")
            f.z.move(Geometry.Z_DOWN_MM)
            return self.Next(self.Step4_WaitAtPlaceDown)

        def Step4_WaitAtPlaceDown(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            self.Describe("lowering to place")
            if f.z.in_position():
                return self.Next(self.Step5_HandRelease)
            return self.Stay()

        def Step5_HandRelease(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            self.Describe("opening gripper")
            f.finger.move(Geometry.FINGER_OPEN_MM)
            if not f.finger.in_position():
                return self.Stay()
            Env.inc_delivered()
            f.carrying = False
            return self.Next(self.Step6_CmdLiftEmpty)

        def Step6_CmdLiftEmpty(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            self.Describe("cmd: lift empty")
            f.z.move(Geometry.Z_UP_MM)
            return self.Next(self.Step7_WaitAtPlaceUp)

        def Step7_WaitAtPlaceUp(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            self.Describe("lifting empty")
            if f.z.in_position():
                return self.Next(self.Step8_CmdRetreat)
            return self.Stay()

        def Step8_CmdRetreat(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            self.Describe("cmd: retreat to C")
            f.x.move(Geometry.ZONE_C_MM)
            return self.Next(self.Step9_WaitAtRetreat)

        def Step9_WaitAtRetreat(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            self.Describe("retreating")
            if f.x.in_position():
                self.Describe("flow done")
                return self.Done()
            return self.Stay()


# ============================================================================
# Flow_Stage - machining cell at zone B. A multi-task module: one flow per part,
# three unit operations - Prepare -> Process -> Cleanup - launched by state.
# ============================================================================

class Flow_Stage(uniflow.Uniflow):
    PROCESS_DURATION_S = 5.0
    TABLE_SPEED_MM_PER_S = 5000.0   # near direct position control for the figure-8

    def __init__(self, rt):
        super().__init__(rt, name="Flow_Stage")
        self._dev = _DeviceClock()
        self.table_x = self._dev.add(MotorAxis("stage_table_x", Geometry.ZONE_B_MM,
                                               self.TABLE_SPEED_MM_PER_S))
        self.hw_ready = self._dev.add(DigitalLatch("stage_hw_ready", 0.2, 0.7))
        self.table_y_offset_mm = 0.0
        self.state_ = StageState.IDLE

        self.ctx_prepare = self.Task_Prepare()
        self.AddTask(self.ctx_prepare)
        self.ctx_process = self.Task_Process()
        self.AddTask(self.ctx_process)
        self.ctx_cleanup = self.Task_Cleanup()
        self.AddTask(self.ctx_cleanup)

    def state(self):
        return self.state_

    def table_x_mm(self):
        return self.table_x.position()

    def table_y_mm(self):
        return self.table_y_offset_mm

    def ready_to_receive_raw_part(self):
        return self.state_ == StageState.IDLE

    def ready_to_hand_off_processed_part(self):
        return self.state_ == StageState.PROCESSED_PART_READY

    def on_raw_part_received(self):
        self.state_ = StageState.RAW_PART_LOADED
        self.Describe("raw part loaded")

    def on_processed_part_taken(self):
        self.state_ = StageState.IDLE
        self.Describe("empty")

    # ----- Task: Prepare - start cmd (async), ack, then wait HW ready -----
    class Task_Prepare(uniflow.Task):
        def __init__(self):
            super().__init__()
            self.settle = None

        def OnEnter(self):
            # re-arm the hw-ready settle timer on the runtime clock.
            self.settle = uniflow.UFTimer(self.flow()._rt.clock)

        def Entry(self):
            return self.Step1_SendStart()

        @staticmethod
        def _simulate_start_cmd():
            time.sleep(0.3)
            return True

        def Step1_SendStart(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            # Guarded bootstrap: only a freshly loaded raw part may begin.
            if f.state_ != StageState.RAW_PART_LOADED:
                return self.Fail()
            self.Describe("send start cmd")
            # async-poll command ack: submit the worker, carry its id to the
            # poller; id 0 means rejected (in-flight cap).
            cmd = self.SubmitAsync(self._simulate_start_cmd, "start_cmd", None)
            if cmd == 0:
                self.Describe("start cmd rejected")
                return self.Fail()
            return self.Next(self.Step2_WaitStartAck, cmd)

        def Step2_WaitStartAck(self, cmd):
            f = self.flow()
            f._dev.tick()
            r = self.AsyncResult(cmd)
            if r.pending():
                self.Describe("wait start ack")
                return self.StayTimeout(2.0, self.Step_StartAckTimeout)
            if not r.ok() or not r.return_value:
                self.Describe("start cmd failed")
                return self.Fail()
            self.Describe("wait hw ready")
            f.hw_ready.arm()
            return self.Next(self.Step3_WaitHwReady)

        def Step_StartAckTimeout(self):
            self.Describe("start ack timeout")
            self.ClearAsync()
            return self.Fail()

        def Step3_WaitHwReady(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            # HeldFor proceeds once ready has held STABLE for 50ms (settling),
            # not on the first transient high. settle was re-armed in OnEnter.
            if self.settle.HeldFor(f.hw_ready.is_ready(), 0.05):
                f.state_ = StageState.PREPARED
                self.Describe("prepared")
                return self.Done()
            self.Describe("wait hw ready")
            return self.StayTimeout(3.0, self.Step4_HwTimeout)

        def Step4_HwTimeout(self):
            self.Describe("hw ready timeout")
            return self.Fail()

    # ----- Task: Process - figure-8 (Gerono lemniscate) machining run -----
    class Task_Process(uniflow.Task):
        def __init__(self):
            super().__init__()
            self.run = None

        def OnEnter(self):
            self.run = uniflow.UFTimer(self.flow()._rt.clock)

        def Entry(self):
            return self.Step1_Process()

        def Step1_Process(self):
            import math
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            elapsed = self.run.Elapsed()
            frac = min(1.0, elapsed / Flow_Stage.PROCESS_DURATION_S)
            # Figure-8: x = sin(t), y = sin(2t). The 2:1 ratio makes the '8'
            # cross at zone centre; four loops over the process duration.
            tau = 6.2831853071795864
            loops = 4
            amp_y = 30.0
            phase = frac * tau * loops
            sweep_mm = Geometry.STAGE_TRAVEL_MM * math.sin(phase)
            f.table_x.move(Geometry.ZONE_B_MM + sweep_mm)
            f.table_y_offset_mm = amp_y * math.sin(phase * 2.0)
            if elapsed >= Flow_Stage.PROCESS_DURATION_S:
                f.state_ = StageState.MACHINED
                self.Describe("machined")
                return self.Done()
            self.Describe("processing")
            return self.Stay()

    # ----- Task: Cleanup - cleanup cmd (async), ack, return to pick pos -----
    class Task_Cleanup(uniflow.Task):
        def Entry(self):
            return self.Step1_SendCleanup()

        @staticmethod
        def _simulate_cleanup_cmd():
            time.sleep(0.2)
            return True

        def Step1_SendCleanup(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            self.Describe("send cleanup cmd")
            cmd = self.SubmitAsync(self._simulate_cleanup_cmd, "cleanup_cmd", None)
            if cmd == 0:
                self.Describe("cleanup cmd rejected")
                return self.Fail()
            return self.Next(self.Step2_WaitCleanupAck, cmd)

        def Step2_WaitCleanupAck(self, cmd):
            f = self.flow()
            f._dev.tick()
            r = self.AsyncResult(cmd)
            if r.pending():
                self.Describe("wait cleanup ack")
                return self.StayTimeout(2.0, self.Step_CleanupAckTimeout)
            if not r.ok() or not r.return_value:
                self.Describe("cleanup failed")
                return self.Fail()
            self.Describe("return to pick pos")
            f.table_x.move(Geometry.ZONE_B_MM)
            return self.Next(self.Step3_ReturnToPickPos)

        def Step_CleanupAckTimeout(self):
            self.Describe("cleanup ack timeout")
            self.ClearAsync()
            return self.Fail()

        def Step3_ReturnToPickPos(self):
            f = self.flow()
            f._dev.tick()
            if Env.stop():
                return self.Done()
            f.table_y_offset_mm = 0.0
            if not f.table_x.in_position():
                return self.Stay()
            f.state_ = StageState.PROCESSED_PART_READY
            self.Describe("ready to hand off")
            return self.Done()


# ============================================================================
# Flow_Orchestrator - line-level scheduler. One perpetual Schedule task whose
# single step polls the line every pump round and launches each module's next
# task when it IsIdle. The pickers / stage never sequence themselves.
# ============================================================================

class Flow_Orchestrator(uniflow.Uniflow):
    def __init__(self, rt):
        super().__init__(rt, name="Flow_Orchestrator")
        self.ctx_schedule = self.Task_Schedule()
        self.AddTask(self.ctx_schedule)

    class Task_Schedule(uniflow.Task):
        def Entry(self):
            return self.Step1_Tick()

        def Step1_Tick(self):
            if Env.stop():
                return self.Done()
            self._try_create_raw_part()
            self._try_drive_load_picker()
            self._try_drive_stage()
            self._try_drive_unload_picker()
            return self.Stay()

        def _try_create_raw_part(self):
            # A fresh raw part is staged the instant zone A is empty.
            if Env.zone_a_has_part():
                return
            Env.create_fake_zone_a_part()

        def _try_drive_load_picker(self):
            picker = App.inst().load
            if not picker.IsIdle():
                return
            # Carrying -> deliver (Place); else grab the next one (Pick).
            if picker.carrying_flag():
                picker.ctx_place.StartFlow()
            elif Env.zone_a_has_part():
                picker.ctx_pick.StartFlow()

        def _try_drive_stage(self):
            stage = App.inst().stage
            if not stage.IsIdle():
                return
            # One task per machining phase, launched as the previous completes.
            st = stage.state()
            if st == StageState.RAW_PART_LOADED:
                stage.ctx_prepare.StartFlow()
            elif st == StageState.PREPARED:
                stage.ctx_process.StartFlow()
            elif st == StageState.MACHINED:
                stage.ctx_cleanup.StartFlow()

        def _try_drive_unload_picker(self):
            picker = App.inst().unload
            if not picker.IsIdle():
                return
            if picker.carrying_flag():
                picker.ctx_place.StartFlow()
                return
            # Prefetch Pick as soon as a part is incoming, so the picker is
            # already hovering above B when the stage finishes.
            st = App.inst().stage.state()
            stage_has_part_incoming = st in (StageState.PREPARED, StageState.MACHINED,
                                             StageState.PROCESSED_PART_READY)
            if stage_has_part_incoming:
                picker.ctx_pick.StartFlow()


# ============================================================================
# Flow_Visualization - pump-side snapshot writer. One perpetual Snapshot task
# copies live line state into g_snap every round (mirrors uf_visualization.cpp).
# ============================================================================

class Flow_Visualization(uniflow.Uniflow):
    def __init__(self, rt):
        super().__init__(rt, name="Flow_Visualization")
        self.ctx_snapshot = self.Task_Snapshot()
        self.AddTask(self.ctx_snapshot)

    class Task_Snapshot(uniflow.Task):
        def Entry(self):
            return self.Step1_Tick()

        def Step1_Tick(self):
            if Env.stop():
                return self.Done()
            app = App.inst()
            load = app.load
            unload = app.unload
            stage = app.stage
            with g_snap_mu:
                g_snap.load_x_mm = load.x_mm()
                g_snap.load_z_mm = load.z_mm()
                g_snap.load_carry = load.carrying_flag()
                g_snap.load_phase = load.CurrentStepDescription()
                g_snap.unload_x_mm = unload.x_mm()
                g_snap.unload_z_mm = unload.z_mm()
                g_snap.unload_carry = unload.carrying_flag()
                g_snap.unload_phase = unload.CurrentStepDescription()
                g_snap.stage_table_x_mm = stage.table_x_mm()
                g_snap.stage_table_y_mm = stage.table_y_mm()
                g_snap.stage_state = stage.state()
                g_snap.stage_phase = stage.CurrentStepDescription()
                g_snap.zone_a_has_part = Env.zone_a_has_part()
                g_snap.delivered = Env.delivered_count()
            return self.Stay()


# ============================================================================
# Console renderer - ANSI side view. A top rail the two pickers hang from, arms
# descending by Z, zones A/B/C + stage table on the track below. Mirrors
# uf_visualization_console.cpp. Render on a background thread ~20 fps; the main
# thread blocks on input() (Enter / EOF quits).
# ============================================================================

K_COLS = 70
K_MARGIN = 4
K_RAIL_ROW = 0
K_Z_ROWS = 6
K_TRACK_ROW = K_RAIL_ROW + K_Z_ROWS + 2
K_ROWS = K_TRACK_ROW + 2
K_HEADER_ROWS = 3


def _col_x(x_mm):
    f = max(0.0, min(1.0, x_mm / Geometry.X_MAX_MM))
    return K_MARGIN + int(f * (K_COLS - 2 * K_MARGIN) + 0.5)


class _Cell:
    __slots__ = ("ch", "color")

    def __init__(self):
        self.ch = " "
        self.color = ""


def _put(cv, r, c, ch, col):
    if r < 0 or r >= K_ROWS or c < 0 or c >= K_COLS:
        return
    cell = cv[r * K_COLS + c]
    cell.ch = ch
    cell.color = col


def _draw_picker(cv, x_mm, z_mm, carry, col):
    c = _col_x(x_mm)
    f = max(0.0, min(1.0, z_mm / Geometry.Z_DOWN_MM))   # 0 up .. 1 fully down
    tip = K_RAIL_ROW + int(f * K_Z_ROWS + 0.5)
    for r in range(K_RAIL_ROW, tip):
        _put(cv, r, c, "|", col)
    _put(cv, tip, c, "#" if carry else "v", col)


def _draw_console(s):
    cv = [_Cell() for _ in range(K_ROWS * K_COLS)]

    ld_col = console.fg(90, 150, 230)    # load picker: blue
    ul_col = console.fg(230, 130, 90)    # unload picker: orange

    # rail across the top
    for c in range(K_MARGIN, K_COLS - K_MARGIN):
        _put(cv, K_RAIL_ROW, c, "=", console.GRAY)

    # track across the bottom, with zone markers
    for c in range(K_MARGIN, K_COLS - K_MARGIN):
        _put(cv, K_TRACK_ROW, c, "=", console.GRAY)

    def mark(x_mm, ch):
        _put(cv, K_TRACK_ROW, _col_x(x_mm), ch, console.BOLD + console.fg(225, 230, 240))

    mark(Geometry.ZONE_A_MM, "A")
    mark(Geometry.ZONE_B_MM, "B")
    mark(Geometry.ZONE_C_MM, "C")

    # raw part waiting in zone A
    if s.zone_a_has_part:
        _put(cv, K_TRACK_ROW - 1, _col_x(Geometry.ZONE_A_MM), "o", console.fg(210, 180, 70))

    # stage table at B, coloured by machining state
    if s.stage_state == StageState.PROCESSED_PART_READY:
        stage_col = console.fg(90, 180, 120)
    elif s.stage_state == StageState.IDLE:
        stage_col = console.fg(120, 124, 134)
    else:
        stage_col = console.fg(210, 130, 60)
    _put(cv, K_TRACK_ROW - 1, _col_x(s.stage_table_x_mm), "#", stage_col)

    # the two pickers hanging from the rail
    _draw_picker(cv, s.load_x_mm, s.load_z_mm, s.load_carry, ld_col)
    _draw_picker(cv, s.unload_x_mm, s.unload_z_mm, s.unload_carry, ul_col)

    # compose one frame, write in a single flush.
    out = []
    out.append(console.at(1, 1) + console.CLEAR_LINE + "  " + console.BOLD
               + "uniflow pick & place  " + console.RESET + console.DIM + "v"
               + uniflow.__version__ + console.RESET)
    out.append(console.at(2, 1) + console.CLEAR_LINE + "  " + "stage: "
               + console.CYAN + s.stage_state + console.RESET + " ("
               + s.stage_phase + ")   delivered: " + console.BOLD
               + str(s.delivered) + console.RESET + "   " + ld_col + "LD"
               + console.RESET + "/" + ul_col + "UL" + console.RESET)
    out.append(console.at(3, 1) + console.CLEAR_LINE + "  " + console.DIM
               + "press Enter to quit" + console.RESET)

    for r in range(K_ROWS):
        line = [console.at(r + 1 + K_HEADER_ROWS, 1) + console.CLEAR_LINE]
        cur = ""
        for c in range(K_COLS):
            cell = cv[r * K_COLS + c]
            if cell.color != cur:
                line.append(console.RESET if cell.color == "" else cell.color)
                cur = cell.color
            line.append(cell.ch)
        line.append(console.RESET)
        out.append("".join(line))

    sys.stdout.write("".join(out))
    sys.stdout.flush()


def run_visualisation_console():
    console.enable_ansi()
    console.hide_cursor()
    console.clear()

    # Render on a background thread; the main thread blocks on stdin so a single
    # Enter (or EOF) quits. The render thread only READS the snapshot.
    done = threading.Event()

    def render_loop():
        while not done.is_set() and not Env.stop():
            _draw_console(read_snapshot())
            time.sleep(0.05)

    render = threading.Thread(target=render_loop, name="pp-render", daemon=True)
    render.start()

    try:
        input()                 # any Enter (or EOF) quits
    except EOFError:
        pass
    done.set()
    render.join(timeout=1.0)

    console.show_cursor()
    sys.stdout.write(console.at(K_ROWS + K_HEADER_ROWS + 2, 1) + console.CLEAR_LINE
                     + "  pick_and_place stopped.\n")
    sys.stdout.flush()


# ============================================================================
# App - holds the Runtime + every module. SILENT observer: the ANSI renderer
# owns stdout, so the framework must not print events.
# ============================================================================

class App:
    _inst = None

    @classmethod
    def inst(cls):
        if cls._inst is None:
            cls._inst = App()
        return cls._inst

    def __init__(self):
        # threads=8 like the C++ port; SILENT observer (base Observer is no-op).
        self.rt = uniflow.Runtime(threads=8, observer=uniflow.Observer())
        self.stage = Flow_Stage(self.rt)
        self.viz = Flow_Visualization(self.rt)
        self.load = Flow_LoadPicker(self.rt)
        self.unload = Flow_UnloadPicker(self.rt)
        self.orch = Flow_Orchestrator(self.rt)

    def start(self):
        self.viz.ctx_snapshot.StartFlow()
        self.orch.ctx_schedule.StartFlow()

    def shutdown(self):
        Env.request_stop()
        self.orch.WaitUntilIdle(timeout=2.0)
        self.load.WaitUntilIdle(timeout=2.0)
        self.unload.WaitUntilIdle(timeout=2.0)
        self.stage.WaitUntilIdle(timeout=2.0)
        self.viz.WaitUntilIdle(timeout=2.0)
        self.rt.stop()


def main():
    app = App.inst()
    app.start()

    run_visualisation_console()

    app.shutdown()

    print("parts delivered to Unload: {}".format(Env.delivered_count()))
    return 0


if __name__ == "__main__":
    sys.exit(main())
