"""message_dispatch.py - one student, two spawners, a shared mailbox.

Python port of cpp/examples/message_dispatch/. A Student worker plus two spawner
modules (Professor and Friend) and a renderer all live on ONE Runtime (one pump
thread). The two spawners push Messages into a shared Mailbox; the Student drains
the mailbox one message at a time and DISPATCHES BY message kind: an Assignment
enters a train/sleep/work chain, a Play enters a play chain.

FEATURE FOCUS:
  - routing by message kind (Step1_TakeNext branches on Message.kind)
  - lock-free mailbox on one pump (all modules share the pump thread, no lock)
  - async poll: blocking "SimHours" goes through SubmitAsync + AsyncResult so the
    pump never blocks; the AsyncId is carried to the wait step via Next(...).

CONSOLE MODE: a Flow_Visualization task snapshots the world each pump round into
g_snap; a background render thread paints an ANSI dashboard ~25 fps while the
main thread blocks on input() (Enter or stdin-EOF quits). The renderer OWNS
stdout, so the Runtime uses a SILENT observer and all activity goes into an
in-memory ring (GlobalLog) shown on the dashboard - never printed directly.

NOTE ON UNITS: the C++ port measures gaps in milliseconds via Clock::now(); the
Python VirtualClock / time.monotonic both return SECONDS, so the same durations
are expressed in seconds here.
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
# globals - shared types, a quit latch, and a recent-activity ring (globals.*)
# ============================================================================

KIND_ASSIGNMENT = "Assignment"
KIND_PLAY = "Play"


class Message:
    """One mailbox item. Assignment uses need_ability / need_time; Play uses
    play_hours."""

    def __init__(self, kind=KIND_ASSIGNMENT, name="",
                 need_ability=0, need_time=0, play_hours=0):
        self.kind = kind
        self.name = name
        self.need_ability = need_ability   # Assignment: required ability
        self.need_time = need_time         # Assignment: hours of actual work
        self.play_hours = play_hours       # Play: hours of playing


# One simulated hour on the pool (seconds). Keeps the demo finite.
K_HOUR_SEC = 0.008
# At or above this stress, the student must sleep before training.
K_STRESS_MAX = 10

# Spawner gaps (seconds), mirroring kProfMinGap.. / kFriendMinGap..
K_PROF_MIN_GAP = 0.200
K_PROF_MAX_GAP = 0.800
K_FRIEND_MIN_GAP = 0.350
K_FRIEND_MAX_GAP = 1.100


# Quit latch: main sets it after Enter/EOF; every step checks it and returns
# Done() so WaitUntilIdle() can return.
g_stop = threading.Event()


# Recent-activity log. Pump-thread steps append lines (a stray stdout print
# would corrupt the ANSI dashboard); the render thread reads the last few for
# the "recent activity" panel - hence the lock.
class GlobalLog:
    _mu = threading.Lock()
    _lines = []
    _CAP = 64

    @classmethod
    def Add(cls, line):
        with cls._mu:
            cls._lines.append(line)
            if len(cls._lines) > cls._CAP:
                del cls._lines[:len(cls._lines) - cls._CAP]

    @classmethod
    def Recent(cls, n):
        with cls._mu:
            return list(cls._lines[-n:])


# ============================================================================
# mailbox - shared inbox for the student (mailbox.*)
# ============================================================================
#
# The two spawners push; the student pops one at a time. All modules are on the
# SAME Runtime, so the mailbox only ever sees the pump thread and needs no lock.

class Mailbox:
    _inbox = []

    @classmethod
    def Push(cls, m):
        cls._inbox.append(m)

    @classmethod
    def TryPop(cls):
        # Returns the front Message, or None if empty.
        if not cls._inbox:
            return None
        return cls._inbox.pop(0)

    @classmethod
    def Size(cls):
        return len(cls._inbox)

    @classmethod
    def ForEach(cls, fn):
        # Walk front-to-back without popping (viz snapshot, same pump thread).
        for m in cls._inbox:
            fn(m)


# ============================================================================
# snapshot - the data the renderer draws each frame (snapshot.*)
# ============================================================================
#
# The viz task on the pump thread writes g_snap under g_snap_mu every round; the
# render thread reads a copy under the same lock. Cross-thread, so it carries a
# lock (unlike the peer reads inside the pump, which need none).

class Snapshot:
    def __init__(self):
        self.queue = []            # mailbox snapshot (front-to-back, not popped)

        self.prof_emitted = 0
        self.prof_total = 0
        self.prof_phase = ""
        self.prof_idle = True

        self.friend_emitted = 0
        self.friend_total = 0
        self.friend_phase = ""
        self.friend_idle = True

        self.student_current = Message()
        self.student_has_msg = False
        self.student_ability = 0
        self.student_stress = 0
        self.student_hours = 0
        self.student_done = 0
        self.student_phase = ""
        self.student_idle = True

        self.recent = []           # recent activity (most recent last)


g_snap_mu = threading.Lock()
g_snap = Snapshot()


def ReadSnapshot():
    with g_snap_mu:
        return g_snap


# ============================================================================
# Flow_Professor - spawner that pushes assignment Messages (uf_professor.*)
# ============================================================================
#
# Emits its preset assignment list into the mailbox at random gaps, waking the
# student after each push, then ends when the list drains.

def _gap(rng, lo, hi):
    return time.monotonic() + rng.uniform(lo, hi)


class Flow_Professor(uniflow.Uniflow):
    def __init__(self, rt, app):
        super().__init__(rt, name="Flow_Professor")
        self.app = app
        self.rng = random.Random(1)
        self.emitted = 0
        self.next_at = 0.0
        self.tasks_list = [
            Message(KIND_ASSIGNMENT, "essay", 2, 4, 0),
            Message(KIND_ASSIGNMENT, "lab", 5, 6, 0),
            Message(KIND_ASSIGNMENT, "project", 8, 9, 0),
            Message(KIND_ASSIGNMENT, "review", 3, 2, 0),
        ]
        self.task_emit = self.Task_Emit()
        self.AddTask(self.task_emit)

    def Emitted(self):
        return self.emitted

    def Total(self):
        return len(self.tasks_list)

    def ScheduleNext(self):
        self.next_at = _gap(self.rng, K_PROF_MIN_GAP, K_PROF_MAX_GAP)

    def EmitOne(self):
        m = self.tasks_list[self.emitted]
        Mailbox.Push(m)   # lock-free: only the pump thread touches the mailbox
        self.emitted += 1
        GlobalLog.Add('professor posted assignment "' + m.name + '"  inbox='
                      + str(Mailbox.Size()))
        self.Describe('posted "', m.name, '"  inbox=', Mailbox.Size())

    class Task_Emit(uniflow.Task):
        def Entry(self):
            return self.Step1_Arm()

        def Step1_Arm(self):
            self.flow().ScheduleNext()
            self.Describe("armed, will emit assignments")
            return self.Next(self.Step2_Tick)

        def Step2_Tick(self):
            f = self.flow()
            if g_stop.is_set():
                return self.Done()
            if f.emitted >= f.Total():
                self.Describe("all assignments handed out")
                return self.Done()
            if time.monotonic() < f.next_at:
                self.Describe("between assignments")
                return self.Stay()   # re-poll this step next round
            f.EmitOne()
            f.ScheduleNext()
            # Wake the student if it parked (cross-module launch via App).
            student = f.app.student
            if student.IsIdle():
                student.task_drain.StartFlow()
            return self.Stay()


# ============================================================================
# Flow_Friend - second spawner: pushes play Messages (uf_friend.*)
# ============================================================================

class Flow_Friend(uniflow.Uniflow):
    def __init__(self, rt, app):
        super().__init__(rt, name="Flow_Friend")
        self.app = app
        self.rng = random.Random(2)
        self.emitted = 0
        self.next_at = 0.0
        self.plays = [
            Message(KIND_PLAY, "soccer", 0, 0, 6),
            Message(KIND_PLAY, "board game", 0, 0, 3),
            Message(KIND_PLAY, "movie", 0, 0, 4),
        ]
        self.task_emit = self.Task_Emit()
        self.AddTask(self.task_emit)

    def Emitted(self):
        return self.emitted

    def Total(self):
        return len(self.plays)

    def ScheduleNext(self):
        self.next_at = _gap(self.rng, K_FRIEND_MIN_GAP, K_FRIEND_MAX_GAP)

    def EmitOne(self):
        m = self.plays[self.emitted]
        Mailbox.Push(m)
        self.emitted += 1
        GlobalLog.Add('friend posted play "' + m.name + '"  inbox='
                      + str(Mailbox.Size()))
        self.Describe('posted "', m.name, '"  inbox=', Mailbox.Size())

    class Task_Emit(uniflow.Task):
        def Entry(self):
            return self.Step1_Arm()

        def Step1_Arm(self):
            self.flow().ScheduleNext()
            self.Describe("armed, will invite to play")
            return self.Next(self.Step2_Tick)

        def Step2_Tick(self):
            f = self.flow()
            if g_stop.is_set():
                return self.Done()
            if f.emitted >= f.Total():
                self.Describe("all invites sent")
                return self.Done()
            if time.monotonic() < f.next_at:
                self.Describe("waiting before next invite")
                return self.Stay()
            f.EmitOne()
            f.ScheduleNext()
            student = f.app.student
            if student.IsIdle():
                student.task_drain.StartFlow()
            return self.Stay()


# ============================================================================
# Flow_Student - the worker that drains the mailbox (uf_student.*)
# ============================================================================
#
# One task: take a Message, dispatch by kind, run the matching chain (train /
# sleep / work for assignments; play for invites), then take the next. It ends
# only when the mailbox is empty AND both spawners are idle - so it survives
# quiet gaps between bursts.

def sim_hours(hours):
    # Static helper for SubmitAsync. Simulates 'hours' of blocking work on a pool
    # thread (never on the pump). Has no access to the flow.
    time.sleep(hours * K_HOUR_SEC)
    return hours


class Flow_Student(uniflow.Uniflow):
    def __init__(self, rt, app):
        super().__init__(rt, name="Flow_Student")
        self.app = app
        self.current = Message()
        self.ability = 0
        self.stress = 0
        self.hours_spent = 0
        self.done_count = 0
        self.task_drain = self.Task_Drain()
        self.AddTask(self.task_drain)

    def Ability(self):
        return self.ability

    def Stress(self):
        return self.stress

    def HoursSpent(self):
        return self.hours_spent

    def DoneCount(self):
        return self.done_count

    def CurrentMessage(self):
        return self.current

    def BothSpawnersDone(self):
        return self.app.prof.IsIdle() and self.app.friend_.IsIdle()

    class Task_Drain(uniflow.Task):
        def Entry(self):
            return self.Step1_TakeNext()

        # --- entry hub: pop one message or park when there is nothing left ---
        def Step1_TakeNext(self):
            f = self.flow()
            if g_stop.is_set():
                return self.Done()
            m = Mailbox.TryPop()
            if m is None:
                if f.BothSpawnersDone():
                    self.Describe("mailbox empty and spawners done -> resting for good")
                    return self.Done()
                # Quiet gap; spawners may still post later. Park-and-wait by
                # Stay()ing rather than Done()ing - the pump just polls us at
                # stay_sleep_sec.
                self.Describe("mailbox empty, spawners not done -> waiting")
                return self.Stay()
            f.current = m

            if m.kind == KIND_ASSIGNMENT:
                # Route by message kind: assignments -> train/sleep/work chain.
                GlobalLog.Add('student took assignment "' + m.name + '"')
                self.Describe('took assignment "', m.name, '"')
                return self.Next(self.Step2_CheckAbility)
            # Plays enter the play chain.
            GlobalLog.Add('student took play "' + m.name + '"')
            self.Describe('took play "', m.name, '"')
            return self.Next(self.Step9_Play)

        # --- assignment chain: train / sleep / do ---
        def Step2_CheckAbility(self):
            f = self.flow()
            if f.ability >= f.current.need_ability:
                self.Describe("ability sufficient -> work")
                return self.Next(self.Step7_Work)
            if f.stress >= K_STRESS_MAX:
                self.Describe("too stressed -> sleep")
                return self.Next(self.Step5_Sleep)
            self.Describe("need more ability -> train")
            return self.Next(self.Step3_Train)

        def Step3_Train(self):
            self.Describe("training (+1 ability, 3h)")
            # Offload the blocking 3h to the pool; carry the AsyncId to the wait.
            job = self.SubmitAsync(sim_hours, "sim_hours", None, 3)
            if job == 0:
                return self.Fail()   # in-flight cap reached
            return self.Next(self.Step4_TrainWait, job)

        def Step4_TrainWait(self, job):
            # Poll the submission by id. While in flight, keep polling this step.
            r = self.AsyncResult(job)
            if r.pending():
                self.Describe("training...")
                return self.Stay()
            if not r.ok():
                return self.Fail()
            f = self.flow()
            f.ability += 1
            f.stress += 1
            f.hours_spent += r.return_value
            GlobalLog.Add("student trained -> ability=" + str(f.ability)
                          + " stress=" + str(f.stress))
            self.Describe("trained: ability=", f.ability, " stress=", f.stress)
            return self.Next(self.Step2_CheckAbility)

        def Step5_Sleep(self):
            self.Describe("sleeping 6h")
            job = self.SubmitAsync(sim_hours, "sim_hours", None, 6)
            if job == 0:
                return self.Fail()
            return self.Next(self.Step6_SleepWait, job)

        def Step6_SleepWait(self, job):
            r = self.AsyncResult(job)
            if r.pending():
                self.Describe("sleeping...")
                return self.Stay()
            if not r.ok():
                return self.Fail()
            f = self.flow()
            f.stress = max(0, f.stress - 6)
            f.hours_spent += r.return_value
            GlobalLog.Add("student slept -> stress=" + str(f.stress))
            self.Describe("slept: stress=", f.stress)
            return self.Next(self.Step2_CheckAbility)

        def Step7_Work(self):
            f = self.flow()
            self.Describe('doing "', f.current.name, '" for ', f.current.need_time, "h")
            job = self.SubmitAsync(sim_hours, "sim_hours", None, f.current.need_time)
            if job == 0:
                return self.Fail()
            return self.Next(self.Step8_WorkWait, job)

        def Step8_WorkWait(self, job):
            r = self.AsyncResult(job)
            if r.pending():
                self.Describe("working...")
                return self.Stay()
            if not r.ok():
                return self.Fail()
            f = self.flow()
            f.hours_spent += r.return_value
            f.stress += 1
            f.done_count += 1
            GlobalLog.Add('student FINISHED assignment "' + f.current.name
                          + '"  (stress=' + str(f.stress)
                          + ", done=" + str(f.done_count) + ")")
            self.Describe('finished "', f.current.name, '"')
            return self.Next(self.Step1_TakeNext)

        # --- play chain: play burns off stress ---
        def Step9_Play(self):
            f = self.flow()
            self.Describe('playing "', f.current.name, '" ', f.current.play_hours, "h")
            job = self.SubmitAsync(sim_hours, "sim_hours", None, f.current.play_hours)
            if job == 0:
                return self.Fail()
            return self.Next(self.Step10_PlayWait, job)

        def Step10_PlayWait(self, job):
            r = self.AsyncResult(job)
            if r.pending():
                self.Describe("playing...")
                return self.Stay()
            if not r.ok():
                return self.Fail()
            f = self.flow()
            relief = r.return_value // 3
            f.stress = max(0, f.stress - relief)
            f.hours_spent += r.return_value
            f.done_count += 1
            GlobalLog.Add('student played "' + f.current.name + '" -> stress='
                          + str(f.stress))
            self.Describe('played "', f.current.name, '"  stress=', f.stress)
            return self.Next(self.Step1_TakeNext)


# ============================================================================
# Flow_Visualization - snapshot the world into g_snap each round (uf_visualization.*)
# ============================================================================
#
# FEATURE FOCUS: a renderer feed is just another flow on the same pump - it reads
# every peer with plain member access (no lock) because they share the thread.
# The actual drawing runs on the background render thread (run_console_renderer).

class Flow_Visualization(uniflow.Uniflow):
    def __init__(self, rt, app):
        super().__init__(rt, name="Flow_Visualization")
        self.app = app
        self.task_snapshot = self.Task_Snapshot()
        self.AddTask(self.task_snapshot)

    class Task_Snapshot(uniflow.Task):
        def Entry(self):
            return self.Step1_Tick()

        def Step1_Tick(self):
            if g_stop.is_set():
                self.Describe("viz stop")
                return self.Done()

            app = self.flow().app
            prof = app.prof
            frnd = app.friend_
            stu = app.student

            s = Snapshot()
            s.prof_emitted = prof.Emitted()
            s.prof_total = prof.Total()
            s.prof_idle = prof.IsIdle()
            s.prof_phase = prof.CurrentStepDescription()

            s.friend_emitted = frnd.Emitted()
            s.friend_total = frnd.Total()
            s.friend_idle = frnd.IsIdle()
            s.friend_phase = frnd.CurrentStepDescription()

            s.student_current = stu.CurrentMessage()
            s.student_has_msg = not stu.IsIdle()
            s.student_ability = stu.Ability()
            s.student_stress = stu.Stress()
            s.student_hours = stu.HoursSpent()
            s.student_done = stu.DoneCount()
            s.student_idle = stu.IsIdle()
            s.student_phase = stu.CurrentStepDescription()

            # Walk the mailbox without popping - safe, same pump thread.
            Mailbox.ForEach(lambda m: s.queue.append(m))

            s.recent = GlobalLog.Recent(6)

            global g_snap
            with g_snap_mu:
                g_snap = s
            return self.Stay()   # re-poll every round to keep it fresh


# ============================================================================
# Render thread: draw an ANSI dashboard from g_snap (uf_visualization.cpp)
# ============================================================================

_SEP = "  " + "-" * 70


def _fmt_msg(m):
    if m.kind == KIND_ASSIGNMENT:
        return ("ASN  " + f"{m.name:<10}" + " need_ability=" + str(m.need_ability)
                + " need_time=" + str(m.need_time) + "h")
    return "PLAY " + f"{m.name:<10}" + " " + str(m.play_hours) + "h"


def _msg_color(m):
    return console.YELLOW if m.kind == KIND_ASSIGNMENT else console.GREEN


def _render(s):
    out = [console.SAVE_CURSOR]
    row = [1]

    def put(text):
        out.append(console.at(row[0], 1) + console.CLEAR_LINE + text)
        row[0] += 1

    put("  " + console.BOLD + "uniflow message_dispatch  " + console.RESET
        + console.DIM + "v" + uniflow.__version__ + console.RESET)
    put("  " + console.DIM
        + "professor + friend -> shared mailbox -> student   "
        + "(one Runtime, one pump, lock-free)" + console.RESET)
    put(_SEP)

    # ---- spawners ----
    prof_done = (s.prof_total > 0 and s.prof_idle
                 and s.prof_emitted >= s.prof_total)
    put("  " + console.YELLOW + "Professor" + console.RESET
        + "  " + str(s.prof_emitted) + "/" + str(s.prof_total) + " sent"
        + ("  (done)" if prof_done else "")
        + "   " + console.DIM + s.prof_phase + console.RESET)

    friend_done = (s.friend_total > 0 and s.friend_idle
                   and s.friend_emitted >= s.friend_total)
    put("  " + console.GREEN + "Friend   " + console.RESET
        + "  " + str(s.friend_emitted) + "/" + str(s.friend_total) + " sent"
        + ("  (done)" if friend_done else "")
        + "   " + console.DIM + s.friend_phase + console.RESET)
    put(_SEP)

    # ---- mailbox ----
    put("  " + console.BOLD + "Mailbox" + console.RESET
        + "  (" + str(len(s.queue)) + " queued)")
    if not s.queue:
        put("    " + console.DIM
            + "(empty - student resting or spawners between bursts)"
            + console.RESET)
    else:
        k_max_shown = 8
        for i, m in enumerate(s.queue[:k_max_shown]):
            put("    " + _msg_color(m) + _fmt_msg(m) + console.RESET)
        if len(s.queue) > k_max_shown:
            put("    " + console.DIM + "... and "
                + str(len(s.queue) - k_max_shown) + " more" + console.RESET)
    # Pad the mailbox region to a fixed floor so the rows below do not jump.
    target = 7 + 9
    while row[0] < target:
        put("")
    put(_SEP)

    # ---- student ----
    if s.student_idle:
        head = console.DIM + "idle" + console.RESET
    else:
        head = ("active: " + _msg_color(s.student_current)
                + _fmt_msg(s.student_current) + console.RESET)
    put("  " + console.BOLD + "Student" + console.RESET + "   " + head)
    put("    " + console.DIM + "phase: " + s.student_phase + console.RESET)

    put("    ability [" + console.CYAN
        + console.bar(s.student_ability / 10.0, 20) + console.RESET
        + "] " + f"{s.student_ability:>2}" + "/10")

    if s.student_stress >= K_STRESS_MAX - 1:
        sc = console.RED
    elif s.student_stress >= K_STRESS_MAX // 2:
        sc = console.YELLOW
    else:
        sc = console.GREEN
    put("    stress  [" + sc
        + console.bar(s.student_stress / K_STRESS_MAX, 20) + console.RESET
        + "] " + f"{s.student_stress:>2}" + "/" + str(K_STRESS_MAX))

    put("    hours spent: " + str(s.student_hours)
        + "        messages handled: " + str(s.student_done))
    put(_SEP)

    # ---- recent activity ----
    put("  " + console.BOLD + "Recent activity" + console.RESET)
    k_lines = 6
    shown = 0
    for line in s.recent:
        put("    " + console.GRAY + line + console.RESET)
        shown += 1
    while shown < k_lines:
        put("")
        shown += 1
    put(_SEP)
    put("  " + console.DIM + "press Enter to quit" + console.RESET)

    out.append(console.RESTORE_CURSOR)
    sys.stdout.write("".join(out))
    sys.stdout.flush()


def run_console_renderer(quit_event):
    # Clears, then redraws the ANSI dashboard ~25 fps from g_snap until quit is
    # set (Enter on the main thread sets it). Owns stdout.
    while not quit_event.is_set():
        _render(ReadSnapshot())
        time.sleep(0.04)   # ~25 fps


# ============================================================================
# App - the Runtime plus all four modules (app.h). Two-phase init.
# ============================================================================

class App:
    def __init__(self):
        # Silent observer: the ANSI renderer OWNS stdout, so the default
        # ConsoleObserver's step-trace output must be suppressed. An empty
        # Observer prints nothing.
        cfg = uniflow.Config(idle_sleep_sec=0.001,
                             stay_sleep_sec=0.02,
                             step_interval_sleep_sec=0.0)
        self.rt = uniflow.Runtime(threads=4,
                                  observer=uniflow.Observer(),
                                  config=cfg)

        # Phase 1: construct (declaration order).
        self.prof = Flow_Professor(self.rt, self)
        self.friend_ = Flow_Friend(self.rt, self)
        self.student = Flow_Student(self.rt, self)
        self.viz = Flow_Visualization(self.rt, self)

    def Start(self):
        # Phase 2: launch. The viz snapshot task and the two spawner tasks start
        # here; the student task is launched on demand by a spawner's first post.
        self.viz.task_snapshot.StartFlow()
        self.prof.task_emit.StartFlow()
        self.friend_.task_emit.StartFlow()

    def Shutdown(self):
        # Set after Enter/EOF; every step checks it and returns Done().
        g_stop.set()
        self.rt.Wake()   # nudge the pump out of any sleep
        self.prof.WaitUntilIdle()
        self.friend_.WaitUntilIdle()
        self.student.WaitUntilIdle()
        self.viz.WaitUntilIdle()
        self.rt.stop()


# ============================================================================
# main - render thread paints; main thread blocks on Enter (main.cpp)
# ============================================================================

def main():
    console.enable_ansi()
    console.hide_cursor()
    console.clear()

    app = App()
    app.Start()

    quit_event = threading.Event()
    renderer = threading.Thread(target=run_console_renderer,
                                args=(quit_event,), daemon=True)
    renderer.start()

    try:
        # Enter (or EOF, e.g. piped input ended) quits.
        try:
            input()
        except EOFError:
            pass
    finally:
        quit_event.set()
        renderer.join(timeout=1.0)
        app.Shutdown()
        console.show_cursor()
        s = app.student
        sys.stdout.write(console.at(40, 1) + console.CLEAR_LINE
                         + "message_dispatch stopped:  hours=" + str(s.HoursSpent())
                         + "  ability=" + str(s.Ability())
                         + "  stress=" + str(s.Stress())
                         + "  messages_handled=" + str(s.DoneCount()) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
