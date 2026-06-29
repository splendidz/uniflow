"""simulator.py - many flows share one pump and one logical clock.

Python port of cpp/examples/simulator/. A console sim where five runner flows
plus a renderer flow all live on ONE Runtime (one pump thread) and one
VirtualClock. Type commands to drive time itself:

  pause       freeze logical time  (runners stop, dashboard stays live)
  resume      resume logical time
  speed <n>   scale logical time   (n > 0; 0.5 = half, 4 = 4x)
  quit        stop and exit

FEATURE FOCUS: VirtualClock (scale / freeze) + single-thread cooperation.
The pump, the five runners, and the renderer all live on one thread; the main
thread does nothing but read stdin and poke the clock (thread-safe).

NOTE ON UNITS: the C++ port measures phases in milliseconds; the Python
VirtualClock.Now() returns SECONDS, so the same durations are expressed in
seconds here (move 3.8 s vs 3800 ms, gate 0.7 s, rest 0.5 s).
"""

import os
import sys
import threading

# --- import shim: make the python/ package dir importable, then sibling console
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import uniflow  # noqa: E402
import console  # noqa: E402  (same dir as this file)


# ============================================================================
# snapshot - the data the renderer draws each frame (mirrors snapshot.h/.cpp)
# ============================================================================
#
# REFERENCE NOTE: there is NO lock here, on purpose. Every Flow_Runner and the
# Flow_View renderer are modules on the SAME Runtime, so they all advance on the
# one pump thread, round-robin. A runner writing its row and the view reading it
# never overlap - that is the core uniflow guarantee: single thread, no locks.
# The only other thread (stdin in main) touches just the clock and g_stop.

K_RUNNER_COUNT = 5

# Fixed dashboard layout (1-based rows). The renderer only ever draws rows
# 1..(K_PROMPT_ROW-1); the stdin prompt lives on K_PROMPT_ROW and is never
# touched by the renderer, so typed input is not clobbered by frame redraws.
K_PROMPT_ROW = 6 + K_RUNNER_COUNT


class RunnerRow:
    """One dashboard line. The runner owns its slot (indexed by ctor id)."""

    def __init__(self):
        self.name = ""
        self.step = "-"      # current step name - the "what is it doing" column
        self.percent = 0.0
        self.lap = 0


# Plain module-level state, pump-thread-only (see note above).
g_rows = [RunnerRow() for _ in range(K_RUNNER_COUNT)]

# Cross-thread shutdown latch: set by the stdin loop, read by every flow's
# steps so they can return Done() and let WaitUntilIdle() return.
g_stop = threading.Event()


# ============================================================================
# Flow_Runner - one simulated worker that laps a track forever (uf_runner.*)
# ============================================================================
#
# FEATURE FOCUS: logical (virtual) time. Progress is measured against
# rt.clock (the VirtualClock) - so every runner speeds up, slows down, and
# freezes together when the user scales or pauses the sim, with no per-flow
# plumbing. The three steps (Gate -> Move -> Rest) loop; each loop is one lap.

class Flow_Runner(uniflow.Uniflow):
    K_GATE_SEC = 0.7   # virtual-time hold at the gate
    K_REST_SEC = 0.5   # virtual-time rest after the line

    def __init__(self, rt, name, idx, move_sec):
        # idx selects this runner's dashboard row; move_sec is the virtual-time
        # length of one Move phase (different per runner for a staggered field).
        super().__init__(rt, name=name)
        self.clock = rt.clock        # bind to the Runtime's logical clock
        self.id = idx
        self.move_sec = move_sec
        self.lap = 0

        self.task_run = self.Task_Run()
        self.AddTask(self.task_run)
        g_rows[self.id].name = name

    class Task_Run(uniflow.Task):
        def OnEnter(self):
            # Re-anchor virtual time whenever the task is entered.
            self.phase_start = self.flow().clock.Now()

        def Entry(self):
            return self.Step1_Gate()

        # Virtual seconds elapsed in the current phase. Because Now() comes from
        # the VirtualClock, this stalls when the sim is paused and stretches /
        # compresses when it is scaled - the whole point of this example.
        def v_elapsed_sec(self):
            return self.flow().clock.Now() - self.phase_start

        def publish(self, percent, step):
            # Plain writes to our own row - same pump thread as the renderer,
            # no lock.
            row = g_rows[self.flow().id]
            row.percent = percent
            row.step = step
            row.lap = self.flow().lap

        def Step1_Gate(self):
            if g_stop.is_set():   # cooperative shutdown so WaitUntilIdle returns
                return self.Done()
            self.publish(0.0, "Step1_Gate")
            if self.v_elapsed_sec() < Flow_Runner.K_GATE_SEC:
                return self.Stay()   # re-poll this step next round
            self.phase_start = self.flow().clock.Now()   # reset for Move phase
            self.Describe("leaving the gate")
            return self.Next(self.Step2_Move)

        def Step2_Move(self):
            if g_stop.is_set():
                return self.Done()
            frac = self.v_elapsed_sec() / self.flow().move_sec
            if frac >= 1.0:
                self.publish(100.0, "Step2_Move")
                self.phase_start = self.flow().clock.Now()
                self.Describe("reached the line")
                return self.Next(self.Step3_Rest)
            self.publish(frac * 100.0, "Step2_Move")
            return self.Stay()

        def Step3_Rest(self):
            if g_stop.is_set():
                return self.Done()
            self.publish(100.0, "Step3_Rest")
            if self.v_elapsed_sec() < Flow_Runner.K_REST_SEC:
                return self.Stay()
            self.flow().lap += 1                  # one full lap completed
            self.phase_start = self.flow().clock.Now()
            return self.Next(self.Step1_Gate)     # loop forever (until g_stop)


# ============================================================================
# Flow_View - the dashboard renderer, itself a uniflow module (uf_view.*)
# ============================================================================
#
# FEATURE FOCUS: a renderer is just another flow on the same pump. It reads
# every runner's row with plain access (no lock) because they share the thread.
# Its frame cadence uses a REAL-time UFTimer (clock=None) - NOT the virtual
# clock - so when the user pauses the sim (clock frozen), the runners stop but
# the dashboard keeps redrawing and shows [PAUSED].

_SEP = "  " + "-" * 60


class Flow_View(uniflow.Uniflow):
    def __init__(self, rt):
        super().__init__(rt, name="Flow_View")
        self.clock = rt.clock        # read scale/frozen state for the header
        self.task_draw = self.Task_Draw()
        self.AddTask(self.task_draw)

    class Task_Draw(uniflow.Task):
        def OnEnter(self):
            # Real-clock throttle (keeps drawing while the sim clock is paused).
            self.fps = uniflow.UFTimer()
            self.fps.Restart()

        def Entry(self):
            return self.Step1_Draw()

        def Step1_Draw(self):
            if g_stop.is_set():
                return self.Done()
            # Throttle to ~30 fps on REAL time. fps is a default UFTimer (wall
            # clock), so the dashboard keeps refreshing even while the sim clock
            # is frozen.
            if self.fps.Passed(0.033):
                self.fps.Restart()
                self.render()
            return self.Stay()

        def render(self):
            out = []

            # Save the user's cursor (sitting on the prompt line), redraw the
            # dashboard above it at fixed positions, then restore it. The prompt
            # row is never touched.
            out.append(console.SAVE_CURSOR)

            def put(row, text):
                out.append(console.at(row, 1) + console.CLEAR_LINE + text)

            put(1, "  " + console.BOLD + "uniflow simulator  " + console.RESET
                   + console.DIM + "v" + uniflow.__version__ + console.RESET)
            put(2, _SEP)

            # Header reads live scale/freeze straight off the VirtualClock.
            clk = self.flow().clock
            if clk.Frozen():
                status = "  " + console.YELLOW + "[PAUSED] " + console.RESET
            else:
                status = "  " + console.GREEN + "[RUNNING]" + console.RESET
            status += ("   speed " + console.CYAN + "x"
                       + f"{clk.Scale():.2f}" + console.RESET
                       + "      " + console.GRAY
                       + "pause | resume | speed <n> | quit" + console.RESET)
            put(3, status)
            put(4, _SEP)

            for i in range(K_RUNNER_COUNT):
                r = g_rows[i]
                line = ("  " + f"{r.name:<8}"
                        + " lap " + f"{r.lap:>2}"
                        + "  [" + console.GREEN + console.bar(r.percent / 100.0, 20)
                        + console.RESET + "] " + f"{int(r.percent + 0.5):>3}" + "%  "
                        + console.DIM + r.step + console.RESET)
                put(5 + i, line)

            put(5 + K_RUNNER_COUNT, _SEP)

            out.append(console.RESTORE_CURSOR)
            sys.stdout.write("".join(out))
            sys.stdout.flush()


# ============================================================================
# App - the Runtime plus every module (app.h). Two-phase init.
# ============================================================================

class App:
    def __init__(self):
        # Silent runtime: this app OWNS the console (the dashboard), so the
        # default ConsoleObserver's trace output must be suppressed. An empty
        # Observer prints nothing.
        #
        # All-Stay rounds (everyone polling) should be short so motion and the
        # ~30 fps renderer stay smooth (sleeps are in SECONDS).
        cfg = uniflow.Config(idle_sleep_sec=0.001,
                             stay_sleep_sec=0.005,
                             step_interval_sleep_sec=0.0)
        self.rt = uniflow.Runtime(observer=uniflow.Observer(), config=cfg)

        # Phase 1: construct. renderer then runners.
        self.view = Flow_View(self.rt)
        self.runners = [
            Flow_Runner(self.rt, "Atlas", 0, 3.8),
            Flow_Runner(self.rt, "Bolt", 1, 2.6),
            Flow_Runner(self.rt, "Comet", 2, 4.6),
            Flow_Runner(self.rt, "Dash", 3, 3.2),
            Flow_Runner(self.rt, "Echo", 4, 5.2),
        ]

    def Start(self):
        # Phase 2: launch every task. Each StartFlow() puts one task on the pump.
        self.view.task_draw.StartFlow()
        for r in self.runners:
            r.task_run.StartFlow()

    def Shutdown(self):
        g_stop.set()       # every step checks this and returns Done()
        self.rt.Wake()     # nudge the pump out of any sleep
        for r in self.runners:
            r.WaitUntilIdle()
        self.view.WaitUntilIdle()
        self.rt.stop()


# ============================================================================
# main - stdin command loop (mirrors main.cpp)
# ============================================================================

def draw_prompt():
    # Draw the input prompt on its reserved row. The renderer never touches this
    # row, so what the user types is not overwritten by frame redraws.
    sys.stdout.write(console.at(K_PROMPT_ROW, 1) + console.CLEAR_LINE + "  "
                     + console.BOLD + "Type a command and press Enter"
                     + console.RESET + console.DIM
                     + " (pause | resume | speed <n> | quit): " + console.RESET)
    sys.stdout.flush()


def run_input_loop(rt):
    while True:
        draw_prompt()
        line = sys.stdin.readline()
        if line == "":
            break   # EOF (e.g. piped input ended) - treat as quit
        parts = line.split()
        if not parts:
            continue
        cmd = parts[0]
        if cmd in ("quit", "exit", "q"):
            break
        if cmd == "pause":
            rt.clock.Freeze()        # stop logical time for ALL runners at once
        elif cmd == "resume":
            rt.clock.Resume()
        elif cmd == "speed":
            if len(parts) >= 2:
                try:
                    n = float(parts[1])
                except ValueError:
                    n = 0.0
                if n > 0.0:
                    rt.clock.SetScale(n)   # one call rescales every flow's pace
        # Unknown commands are ignored; the dashboard keeps running.


def main():
    console.enable_ansi()
    console.hide_cursor()
    console.clear()

    app = App()
    app.Start()

    try:
        run_input_loop(app.rt)
    finally:
        app.Shutdown()
        console.show_cursor()
        sys.stdout.write(console.at(K_PROMPT_ROW + 1, 1) + console.CLEAR_LINE
                         + "  simulator stopped.\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
