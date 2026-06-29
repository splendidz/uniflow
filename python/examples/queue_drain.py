"""queue_drain.py - lock-free single-pump producer / consumer.

Python port of cpp/examples/queue_drain/. A Flow_Sender module pushes random
arithmetic jobs into a shared mailbox in bursts; a Flow_Receiver module drains
the mailbox one job at a time, dispatching by operator. Both modules live on ONE
Runtime (one pump thread), so the mailbox needs no lock between them - a
lock-free single-pump producer / consumer.

FEATURE FOCUS: park / relaunch wake. When the queue empties, the receiver
returns Done() and its module PARKS (goes idle). The sender, on its next burst,
sees recv.IsIdle() and relaunches the drain task with task.StartFlow() - the
classic IsIdle() + StartFlow() wake pattern. Both calls happen inline on the
same pump thread, so they are plain in-thread calls, no lock, no cross-thread
signal.

The only cross-thread synchronisation in the whole demo is the snapshot mutex:
a Flow_Visualization snapshot step (pump thread) copies live state into g_snap
under g_snap_mu; a background render thread reads it under the same mutex and
draws the ANSI dashboard ~25 fps. The main thread blocks on input() so a single
Enter (or EOF) quits. SILENT observer - the app owns the console.

NOTE ON UNITS: the C++ port uses milliseconds; Python UFTimer.Passed() takes
SECONDS, so the same durations are expressed in seconds here (send gap 0.6 s,
render frame 0.04 s).
"""

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
# globals - shared value type, tuning constants, cross-thread stop latch
# ============================================================================
#
# FEATURE FOCUS: the sender and the receiver run as two modules on ONE pump
# thread, so the mailbox between them needs no lock at all. This section only
# holds the value type they exchange, the demo's tuning constants, and the
# cross-thread stop flag the stdin loop sets.

class Msg:
    """One arithmetic job: the sender pushes it, the receiver pops/evaluates it."""

    def __init__(self, a=0, b=0, op="+"):
        self.a = a
        self.b = b
        self.op = op


class GlobalConfig:
    K_VEC_SIZE = 10
    K_VALUE_MAX = 30
    K_BURST_MIN = 1
    K_BURST_MAX = 10
    K_SEND_GAP = 0.6        # seconds between bursts (C++ 600ms)
    K_MAX_BURST_COUNT = 20  # stop the sender here


# Process-wide stop latch. The stdin loop sets it; every step polls it and
# returns Done(), which lets WaitUntilIdle() return at shutdown.
g_stop = threading.Event()


# ============================================================================
# mailbox - FIFO the sender enqueues into and the receiver drains (mailbox.*)
# ============================================================================
#
# Sender + receiver both live on the same Runtime, so the mailbox is touched
# only by one pump thread - it is a plain list, no lock. The viz snapshot step
# also runs on the pump thread, so Snapshot() is lock-free too; the only lock in
# the demo guards g_snap (read on the main / render thread).

class Mailbox:
    _queue = []   # pump-thread-only deque (list used as FIFO)

    @staticmethod
    def push(m):
        Mailbox._queue.append(m)

    @staticmethod
    def try_pop():
        # Returns the next Msg or None. Lock-free: only the pump thread touches it.
        if not Mailbox._queue:
            return None
        return Mailbox._queue.pop(0)

    @staticmethod
    def size():
        return len(Mailbox._queue)

    @staticmethod
    def snapshot():
        # Copy the queued items for the visualisation. Called on the pump thread.
        return list(Mailbox._queue)


# ============================================================================
# snapshot - one frame of state for the renderer (snapshot.*)
# ============================================================================
#
# The Flow_Visualization snapshot step (pump thread) writes g_snap under
# g_snap_mu; the background render thread reads it under the same mutex. That
# mutex is the demo's ONLY cross-thread synchronisation - the mailbox itself is
# lock-free because the sender and receiver share the pump thread.

# Receiver states (uf_receiver enum mirror).
STATE_IDLE = "Idle"
STATE_DISPATCHING = "Dispatching"
STATE_ADDING = "Adding"
STATE_SUBTRACTING = "Subtracting"


class Snapshot:
    def __init__(self):
        self.vec_a = []
        self.vec_b = []
        self.queue = []
        self.last_burst_count = 0
        self.total_bursts = 0

        self.recv_state = STATE_IDLE
        self.processed = 0
        self.last_result = ""
        self.current = Msg()

        self.sender_phase = ""
        self.recv_phase = ""

        # Per-job cycle speed: how fast the receiver drains jobs.
        self.last_cycle_ms = 0.0    # wall time the most recent job took (ms)
        self.jobs_per_sec = 0.0     # instantaneous rate = 1000 / last_cycle_ms
        self.avg_ms_per_job = 0.0   # overall average = elapsed_ms / processed


g_snap_mu = threading.Lock()
g_snap = Snapshot()


def read_snapshot():
    # By-value snapshot read under the mutex (called on the render / main thread).
    with g_snap_mu:
        s = Snapshot()
        s.vec_a = list(g_snap.vec_a)
        s.vec_b = list(g_snap.vec_b)
        s.queue = list(g_snap.queue)
        s.last_burst_count = g_snap.last_burst_count
        s.total_bursts = g_snap.total_bursts
        s.recv_state = g_snap.recv_state
        s.processed = g_snap.processed
        s.last_result = g_snap.last_result
        s.current = g_snap.current
        s.sender_phase = g_snap.sender_phase
        s.recv_phase = g_snap.recv_phase
        s.last_cycle_ms = g_snap.last_cycle_ms
        s.jobs_per_sec = g_snap.jobs_per_sec
        s.avg_ms_per_job = g_snap.avg_ms_per_job
        return s


# ============================================================================
# Flow_Sender - the feed module that stuffs the inbox in bursts (uf_sender.*)
# ============================================================================
#
# One perpetual task (Emit): every K_SEND_GAP, push a random burst of 1..N jobs
# into the mailbox, then relaunch the receiver if it has parked. Because both
# modules run on the same pump thread, pushing to the mailbox and calling
# recv.IsIdle() / StartFlow() inline are all lock-free, in-thread calls.

class Flow_Sender(uniflow.Uniflow):
    def __init__(self, rt):
        super().__init__(rt, name="Flow_Sender")
        self.rng = random.Random()

        # Flow-owned state, reached from the step via self.flow().member.
        self.vec_a = []
        self.vec_b = []
        self.last_burst_count = 0
        self.total_bursts = 0

        self.fill_vectors()

        # The receiver, wired by the App after construction (relaunch target).
        self.recv = None

        self.task_emit = self.Task_Emit()
        self.AddTask(self.task_emit)

    def fill_vectors(self):
        for _ in range(GlobalConfig.K_VEC_SIZE):
            self.vec_a.append(self.rng.randint(1, GlobalConfig.K_VALUE_MAX))
            self.vec_b.append(self.rng.randint(1, GlobalConfig.K_VALUE_MAX))

    def emit_burst(self):
        rng = self.rng
        n = rng.randint(GlobalConfig.K_BURST_MIN, GlobalConfig.K_BURST_MAX)
        self.last_burst_count = n
        self.total_bursts += 1
        for _ in range(n):
            m = Msg()
            m.a = self.vec_a[rng.randint(0, GlobalConfig.K_VEC_SIZE - 1)]
            m.b = self.vec_b[rng.randint(0, GlobalConfig.K_VEC_SIZE - 1)]
            m.op = "+" if rng.randint(0, 1) == 0 else "-"
            Mailbox.push(m)   # lock-free: only this pump thread touches the queue
        return n

    class Task_Emit(uniflow.Task):
        def OnEnter(self):
            # Re-arm the burst timer on task entry (real wall clock).
            self.gap = uniflow.UFTimer()
            self.gap.Restart()

        def Entry(self):
            return self.Step1_Tick()

        def Step1_Tick(self):
            f = self.flow()
            if g_stop.is_set():
                self.Describe("stop requested -> done")
                return self.Done()
            if f.total_bursts >= GlobalConfig.K_MAX_BURST_COUNT:
                # Burst budget spent: stop emitting so the demo settles. The
                # receiver drains the final burst and parks; dashboard runs on.
                self.Describe("burst budget exhausted -> done")
                return self.Done()

            # Throttle on wall time. gap was armed by OnEnter and survives Stay
            # re-entries, so Passed() measures from task entry / last Restart.
            if not self.gap.Passed(GlobalConfig.K_SEND_GAP):
                self.Describe("idle gap")
                return self.Stay()   # re-poll this step next round

            n = f.emit_burst()
            self.gap.Restart()
            self.Describe("burst pushed: ", n, " jobs (queue=", Mailbox.size(), ")")

            # Wake the receiver if it has parked. Same pump thread, so IsIdle()
            # and StartFlow() are plain in-thread calls - no lock, no signal.
            if f.recv.IsIdle():
                f.recv.task_drain.StartFlow()

            return self.Stay()


# ============================================================================
# Flow_Receiver - the worker module that drains the inbox (uf_receiver.*)
# ============================================================================
#
# One looping task (Drain): pop one Msg, dispatch to the Add or Sub step by its
# operator, then loop back to pop the next. When the queue empties the task
# Done()s and the module PARKS; the sender relaunches it on the next burst.
# State the steps share (current job, counters) lives on the flow.

class Flow_Receiver(uniflow.Uniflow):
    def __init__(self, rt):
        super().__init__(rt, name="Flow_Receiver")

        # Flow-owned state, reached from the steps via self.flow().member.
        self.state = STATE_IDLE
        self.current = Msg()
        self.processed = 0
        self.last_result = ""

        self.task_drain = self.Task_Drain()
        self.AddTask(self.task_drain)

    class Task_Drain(uniflow.Task):
        def Entry(self):
            return self.Step1_TakeNext()

        def Step1_TakeNext(self):
            f = self.flow()
            if g_stop.is_set():
                return self.Done()
            f.state = STATE_DISPATCHING

            # Pop one job. Mailbox is touched only on this pump thread, no lock.
            m = Mailbox.try_pop()
            if m is None:
                # Queue drained: park the module. Done() lets it idle until the
                # sender relaunches this task (task.StartFlow()) on the next burst.
                f.state = STATE_IDLE
                self.Describe("queue drained -> done")
                return self.Done()
            f.current = m
            self.Describe("popped ", m.a, " ", m.op, " ", m.b)

            # Dispatch by operator: Next() routes to a sibling step in this task.
            if m.op == "+":
                return self.Next(self.Step2_Add)
            return self.Next(self.Step3_Sub)

        def Step2_Add(self):
            f = self.flow()
            f.state = STATE_ADDING
            result = f.current.a + f.current.b
            f.last_result = f"{f.current.a} + {f.current.b} = {result}"
            f.processed += 1
            self.Describe("add: ", f.last_result)
            return self.Next(self.Step1_TakeNext)   # loop back for the next job

        def Step3_Sub(self):
            f = self.flow()
            f.state = STATE_SUBTRACTING
            result = f.current.a - f.current.b
            f.last_result = f"{f.current.a} - {f.current.b} = {result}"
            f.processed += 1
            self.Describe("sub: ", f.last_result)
            return self.Next(self.Step1_TakeNext)


# ============================================================================
# Flow_Visualization - pump-side snapshot writer (uf_visualization.*)
# ============================================================================
#
# A module on the pump thread whose one perpetual step copies live sender /
# receiver / mailbox state into g_snap each tick, under g_snap_mu, so the render
# thread always sees a consistent frame. This is a perpetual poll - it ends only
# on stop.

class Flow_Visualization(uniflow.Uniflow):
    def __init__(self, rt):
        super().__init__(rt, name="Flow_Visualization")
        # Wired by the App after construction (read-only, same pump thread).
        self.send = None
        self.recv = None

        self.task_snapshot = self.Task_Snapshot()
        self.AddTask(self.task_snapshot)

    class Task_Snapshot(uniflow.Task):
        def OnEnter(self):
            # Per-job cycle-speed measurement state (real wall clock).
            self.started = False
            self.start = 0.0
            self.last_job = 0.0
            self.last_processed = 0
            self.last_cycle_ms = 0.0

        def Entry(self):
            return self.Step1_Tick()

        def Step1_Tick(self):
            f = self.flow()
            if g_stop.is_set():
                return self.Done()
            s = f.send
            r = f.recv

            # Measure the per-job cycle speed: how long the receiver takes
            # between consecutive job completions, plus the overall average.
            now = time.perf_counter()
            if not self.started:
                self.start = now
                self.last_job = now
                self.started = True
            processed = r.processed
            if processed > self.last_processed:
                dt = (now - self.last_job) * 1000.0
                self.last_cycle_ms = dt / (processed - self.last_processed)
                self.last_job = now
                self.last_processed = processed
            avg_ms = (now - self.start) * 1000.0 / processed if processed > 0 else 0.0
            jps = 1000.0 / self.last_cycle_ms if self.last_cycle_ms > 0.0 else 0.0

            with g_snap_mu:
                g_snap.last_cycle_ms = self.last_cycle_ms
                g_snap.jobs_per_sec = jps
                g_snap.avg_ms_per_job = avg_ms
                g_snap.vec_a = list(s.vec_a)
                g_snap.vec_b = list(s.vec_b)
                g_snap.queue = Mailbox.snapshot()
                g_snap.last_burst_count = s.last_burst_count
                g_snap.total_bursts = s.total_bursts
                g_snap.recv_state = r.state
                g_snap.processed = r.processed
                g_snap.last_result = r.last_result
                g_snap.current = r.current
                g_snap.sender_phase = s.CurrentStepDescription()
                g_snap.recv_phase = r.CurrentStepDescription()
            return self.Stay()


# ============================================================================
# render side - background-thread ANSI dashboard (uf_visualization.cpp)
# ============================================================================

_SEP = "  " + "-" * 60


def _format_msg(m):
    return f"{m.a} {m.op} {m.b}"


def _state_color(state):
    if state == STATE_ADDING:
        return console.fg(90, 180, 120)
    if state == STATE_SUBTRACTING:
        return console.fg(210, 130, 60)
    if state == STATE_DISPATCHING:
        return console.fg(200, 190, 80)
    if state == STATE_IDLE:
        return console.fg(120, 124, 134)
    return console.RESET


def _draw_console(s):
    out = []

    def put(row, text):
        out.append(console.at(row, 1) + console.CLEAR_LINE + text)

    put(1, "  " + console.BOLD + "uniflow queue_drain  " + console.RESET
           + console.DIM + "v" + uniflow.__version__ + console.RESET)
    put(2, _SEP)

    # sender line: burst counters + current step description.
    put(3, "  " + console.CYAN + "sender" + console.RESET
           + "   bursts " + console.BOLD + str(s.total_bursts) + "/"
           + str(GlobalConfig.K_MAX_BURST_COUNT) + console.RESET
           + "   last burst " + str(s.last_burst_count)
           + "   " + console.DIM + s.sender_phase + console.RESET)

    # source vectors the sender draws operands from.
    a = "  vec A:"
    b = "  vec B:"
    for v in s.vec_a:
        a += " " + f"{v:>2}"
    for v in s.vec_b:
        b += " " + f"{v:>2}"
    put(4, console.GRAY + a + console.RESET)
    put(5, console.GRAY + b + console.RESET)

    put(6, _SEP)

    # queue depth + chip view of the pending jobs (lock-free list snapshot).
    put(7, "  " + console.YELLOW + "queue" + console.RESET
           + "    depth " + console.BOLD + str(len(s.queue)) + console.RESET)

    chips = "  "
    shown = 0
    while shown < len(s.queue) and shown < 12:
        chips += "[" + _format_msg(s.queue[shown]) + "] "
        shown += 1
    if len(s.queue) > shown:
        chips += "+" + str(len(s.queue) - shown) + " more"
    if not s.queue:
        chips += console.DIM + "(empty)" + console.RESET
    put(8, chips)

    put(9, _SEP)

    # receiver line: state chip + processed count + current step description.
    put(10, "  " + _state_color(s.recv_state) + "receiver " + s.recv_state
            + console.RESET + "   processed " + console.BOLD + str(s.processed)
            + console.RESET + "   " + console.DIM + s.recv_phase + console.RESET)
    put(11, "  last result: " + console.BOLD
            + (s.last_result if s.last_result else "-") + console.RESET)

    # cycle speed: how fast the receiver drains one job at a time.
    put(12, "  " + console.CYAN + "cycle" + console.RESET
            + "    last " + console.BOLD + f"{s.last_cycle_ms:.1f}" + " ms/job"
            + console.RESET + "   " + f"{s.jobs_per_sec:.1f}" + " jobs/s"
            + "   " + console.DIM + "avg " + f"{s.avg_ms_per_job:.1f}" + " ms/job"
            + console.RESET)

    put(13, _SEP)
    put(14, "  " + console.DIM + "press Enter to quit" + console.RESET)

    sys.stdout.write("".join(out))
    sys.stdout.flush()


_K_STATUS_ROW = 15


def run_visualisation():
    console.enable_ansi()
    console.hide_cursor()
    console.clear()

    # Render on a background thread; the main thread blocks on stdin so a single
    # Enter quits. The render thread only READS the snapshot (mutex-guarded).
    done = threading.Event()

    def render_loop():
        while not done.is_set() and not g_stop.is_set():
            _draw_console(read_snapshot())
            done.wait(0.04)   # ~25 fps

    render = threading.Thread(target=render_loop, name="qd-render", daemon=True)
    render.start()

    try:
        sys.stdin.readline()   # any Enter (or EOF) quits
    except (EOFError, KeyboardInterrupt):
        pass
    done.set()
    render.join()

    console.show_cursor()
    sys.stdout.write(console.at(_K_STATUS_ROW, 1) + console.CLEAR_LINE
                     + "  queue_drain stopped.\n")
    sys.stdout.flush()


# ============================================================================
# App - the Runtime plus every module (app.h). Two-phase init.
# ============================================================================
#
# REFERENCE NOTE: one Runtime, one pump thread, three flows (sender, receiver,
# renderer). They cooperate without a single lock on the mailbox between them -
# the only mutex in the demo guards the render-thread snapshot.

class App:
    def __init__(self):
        # Silent runtime: this app OWNS the console (the dashboard), so the
        # default ConsoleObserver's trace output must be suppressed. An empty
        # Observer prints nothing. Sleep knobs (seconds): burst on any-Next round,
        # short stay nap, tiny idle nap.
        cfg = uniflow.Config(idle_sleep_sec=0.001,
                             stay_sleep_sec=0.02,
                             step_interval_sleep_sec=0.0)
        self.rt = uniflow.Runtime(observer=uniflow.Observer(), config=cfg)

        # Phase 1: construct every module.
        self.send = Flow_Sender(self.rt)
        self.recv = Flow_Receiver(self.rt)
        self.viz = Flow_Visualization(self.rt)

        # Cross-module wiring (safe now that all modules exist).
        self.send.recv = self.recv
        self.viz.send = self.send
        self.viz.recv = self.recv

    def Start(self):
        # Phase 2: launch the perpetual tasks. The receiver is NOT started here -
        # the sender relaunches its drain task on the first burst.
        self.viz.task_snapshot.StartFlow()
        self.send.task_emit.StartFlow()

    def Shutdown(self):
        g_stop.set()       # every step polls this and returns Done()
        self.rt.Wake()     # nudge the pump out of any sleep
        self.send.WaitUntilIdle()
        self.recv.WaitUntilIdle()
        self.viz.WaitUntilIdle()
        self.rt.stop()


# ============================================================================
# main - mirrors main.cpp
# ============================================================================

def main():
    app = App()        # Phase 1: every module is now constructed.
    app.Start()        # Phase 2: flows start; cross-module refs safe.

    run_visualisation()  # main-thread render loop (background draw + stdin).

    app.Shutdown()

    s = read_snapshot()
    print(f"  bursts sent: {s.total_bursts}   jobs processed: {s.processed}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
