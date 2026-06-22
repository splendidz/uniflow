// Flows.cs - the four modules on the one Runtime: Professor and Friend (spawners),
// Student (the worker that drains the mailbox and routes by message kind), and
// Visualization (snapshots the world for the render thread).
//
// FEATURE FOCUS:
//   - routing by message kind (Step1_TakeNext branches on Message.Kind)
//   - lock-free mailbox on one pump (all modules share the pump thread, no lock)
//   - async poll: blocking "SimHours" goes through SubmitAsync + AsyncResult so the
//     pump never blocks; the AsyncId is carried to the wait step in an instance field.
//
// Cross-module state is reached via Flow.App (each module is handed the shared App
// in its ctor) - the same pattern the Python port uses.

#nullable enable

using System;
using System.Threading;
using Uniflow;

namespace Uniflow.Examples.MessageDispatch
{
    // ----- App: the Runtime plus all four modules (app.h). Two-phase init. -----
    public sealed class App
    {
        public readonly Runtime Rt;
        public readonly Flow_Professor Prof;
        public readonly Flow_Friend Friend;
        public readonly Flow_Student Student;
        public readonly Flow_Visualization Viz;

        public App()
        {
            // Silent observer: the ANSI renderer OWNS stdout, so the default
            // ConsoleObserver's step-trace output must be suppressed. A plain
            // 'new Observer()' prints nothing.
            var cfg = new Config
            {
                IdleSleep = 0.001,
                StaySleep = 0.02,
                StepIntervalSleep = 0.0,
            };
            Rt = new Runtime(new Runtime.Options { Threads = 4, Observer = new Observer(), Config = cfg });

            // Phase 1: construct (declaration order). Each module gets the App so it
            // can reach its peers.
            Prof = new Flow_Professor(Rt, this);
            Friend = new Flow_Friend(Rt, this);
            Student = new Flow_Student(Rt, this);
            Viz = new Flow_Visualization(Rt, this);
        }

        public void Start()
        {
            // Phase 2: launch. The viz snapshot task and the two spawner tasks start
            // here; the student task is launched on demand by a spawner's first post.
            Viz.CtxSnapshot.StartFlow();
            Prof.CtxEmit.StartFlow();
            Friend.CtxEmit.StartFlow();
        }

        public void Shutdown()
        {
            // Set after Enter/EOF; every step checks it and returns Done().
            GlobalEnv.SetStop();
            Rt.Wake();   // nudge the pump out of any sleep
            Prof.WaitUntilIdle();
            Friend.WaitUntilIdle();
            Student.WaitUntilIdle();
            Viz.WaitUntilIdle();
            Rt.Stop();
        }
    }

    // ----- SimWork: blocking helper for SubmitAsync -----
    // Simulates 'hours' of blocking work on a pool thread (never on the pump). Has
    // no access to the flow.
    public static class SimWork
    {
        public static int SimHours(int hours)
        {
            Thread.Sleep(TimeSpan.FromSeconds(hours * GlobalConfig.HourSec));
            return hours;
        }
    }

    // ========================================================================
    // Flow_Professor - spawner that pushes assignment Messages.
    // ========================================================================
    //
    // Emits its preset assignment list into the mailbox at random gaps, waking the
    // student after each push, then ends when the list drains.
    public sealed class Flow_Professor : Module
    {
        public readonly App App;
        private readonly Random _rng = new Random(1);
        private int _emitted;
        private double _nextAt;
        private readonly Message[] _tasks = new[]
        {
            new Message(MessageKind.Assignment, "essay", 2, 4, 0),
            new Message(MessageKind.Assignment, "lab", 5, 6, 0),
            new Message(MessageKind.Assignment, "project", 8, 9, 0),
            new Message(MessageKind.Assignment, "review", 3, 2, 0),
        };

        public readonly Task_Emit CtxEmit;

        public Flow_Professor(Runtime rt, App app) : base(rt, "Flow_Professor")
        {
            App = app;
            CtxEmit = new Task_Emit();
            AddTask(CtxEmit);
        }

        public int Emitted => _emitted;
        public int Total => _tasks.Length;

        private void ScheduleNext()
        {
            _nextAt = MonotonicNow() + _rng.NextDouble() * (GlobalConfig.ProfMaxGap - GlobalConfig.ProfMinGap)
                      + GlobalConfig.ProfMinGap;
        }

        private void EmitOne()
        {
            Message m = _tasks[_emitted];
            Mailbox.Push(m);   // lock-free: only the pump thread touches the mailbox
            _emitted++;
            GlobalLog.Add("professor posted assignment \"" + m.Name + "\"  inbox=" + Mailbox.Size());
        }

        private static double MonotonicNow()
        {
            return System.Diagnostics.Stopwatch.GetTimestamp() / (double)System.Diagnostics.Stopwatch.Frequency;
        }

        public sealed class Task_Emit : Task<Flow_Professor>
        {
            protected override StepResult Entry() => Step1_Arm();

            private StepResult Step1_Arm()
            {
                Flow.ScheduleNext();
                Describe("armed, will emit assignments");
                return Next(Step2_Tick);
            }

            private StepResult Step2_Tick()
            {
                if (GlobalEnv.Stop())
                {
                    return Done();
                }
                if (Flow._emitted >= Flow.Total)
                {
                    Describe("all assignments handed out");
                    return Done();
                }
                if (MonotonicNow() < Flow._nextAt)
                {
                    Describe("between assignments");
                    return Stay();   // re-poll this step next round
                }
                Flow.EmitOne();
                Flow.ScheduleNext();
                // Wake the student if it parked (cross-module launch via App).
                Flow_Student student = Flow.App.Student;
                if (student.IsIdle)
                {
                    student.CtxDrain.StartFlow();
                }
                return Stay();
            }
        }
    }

    // ========================================================================
    // Flow_Friend - second spawner: pushes play Messages.
    // ========================================================================
    public sealed class Flow_Friend : Module
    {
        public readonly App App;
        private readonly Random _rng = new Random(2);
        private int _emitted;
        private double _nextAt;
        private readonly Message[] _plays = new[]
        {
            new Message(MessageKind.Play, "soccer", 0, 0, 6),
            new Message(MessageKind.Play, "board game", 0, 0, 3),
            new Message(MessageKind.Play, "movie", 0, 0, 4),
        };

        public readonly Task_Emit CtxEmit;

        public Flow_Friend(Runtime rt, App app) : base(rt, "Flow_Friend")
        {
            App = app;
            CtxEmit = new Task_Emit();
            AddTask(CtxEmit);
        }

        public int Emitted => _emitted;
        public int Total => _plays.Length;

        private void ScheduleNext()
        {
            _nextAt = MonotonicNow() + _rng.NextDouble() * (GlobalConfig.FriendMaxGap - GlobalConfig.FriendMinGap)
                      + GlobalConfig.FriendMinGap;
        }

        private void EmitOne()
        {
            Message m = _plays[_emitted];
            Mailbox.Push(m);
            _emitted++;
            GlobalLog.Add("friend posted play \"" + m.Name + "\"  inbox=" + Mailbox.Size());
        }

        private static double MonotonicNow()
        {
            return System.Diagnostics.Stopwatch.GetTimestamp() / (double)System.Diagnostics.Stopwatch.Frequency;
        }

        public sealed class Task_Emit : Task<Flow_Friend>
        {
            protected override StepResult Entry() => Step1_Arm();

            private StepResult Step1_Arm()
            {
                Flow.ScheduleNext();
                Describe("armed, will invite to play");
                return Next(Step2_Tick);
            }

            private StepResult Step2_Tick()
            {
                if (GlobalEnv.Stop())
                {
                    return Done();
                }
                if (Flow._emitted >= Flow.Total)
                {
                    Describe("all invites sent");
                    return Done();
                }
                if (MonotonicNow() < Flow._nextAt)
                {
                    Describe("waiting before next invite");
                    return Stay();
                }
                Flow.EmitOne();
                Flow.ScheduleNext();
                Flow_Student student = Flow.App.Student;
                if (student.IsIdle)
                {
                    student.CtxDrain.StartFlow();
                }
                return Stay();
            }
        }
    }

    // ========================================================================
    // Flow_Student - the worker that drains the mailbox.
    // ========================================================================
    //
    // One task: take a Message, dispatch by kind, run the matching chain (train /
    // sleep / work for assignments; play for invites), then take the next. It ends
    // only when the mailbox is empty AND both spawners are idle - so it survives
    // quiet gaps between bursts.
    public sealed class Flow_Student : Module
    {
        public readonly App App;
        private Message _current = new Message();
        private int _ability;
        private int _stress;
        private int _hoursSpent;
        private int _doneCount;

        public readonly Task_Drain CtxDrain;

        public Flow_Student(Runtime rt, App app) : base(rt, "Flow_Student")
        {
            App = app;
            CtxDrain = new Task_Drain();
            AddTask(CtxDrain);
        }

        public int Ability => _ability;
        public int Stress => _stress;
        public int HoursSpent => _hoursSpent;
        public int DoneCount => _doneCount;
        public Message CurrentMessage => _current;

        private bool BothSpawnersDone()
        {
            return App.Prof.IsIdle && App.Friend.IsIdle;
        }

        public sealed class Task_Drain : Task<Flow_Student>
        {
            // The AsyncId of the in-flight SimHours, carried to the matching wait step.
            private int _job;

            protected override StepResult Entry() => Step1_TakeNext();

            // --- entry hub: pop one message or park when there is nothing left ---
            private StepResult Step1_TakeNext()
            {
                if (GlobalEnv.Stop())
                {
                    return Done();
                }
                Message? m = Mailbox.TryPop();
                if (m == null)
                {
                    if (Flow.BothSpawnersDone())
                    {
                        Describe("mailbox empty and spawners done -> resting for good");
                        return Done();
                    }
                    // Quiet gap; spawners may still post later. Park-and-wait by
                    // Stay()ing rather than Done()ing - the pump just polls us.
                    Describe("mailbox empty, spawners not done -> waiting");
                    return Stay();
                }
                Flow._current = m;

                if (m.Kind == MessageKind.Assignment)
                {
                    // Route by message kind: assignments -> train/sleep/work chain.
                    GlobalLog.Add("student took assignment \"" + m.Name + "\"");
                    Describe("took assignment \"" + m.Name + "\"");
                    return Next(Step2_CheckAbility);
                }
                // Plays enter the play chain.
                GlobalLog.Add("student took play \"" + m.Name + "\"");
                Describe("took play \"" + m.Name + "\"");
                return Next(Step9_Play);
            }

            // --- assignment chain: train / sleep / do ---
            private StepResult Step2_CheckAbility()
            {
                if (Flow._ability >= Flow._current.NeedAbility)
                {
                    Describe("ability sufficient -> work");
                    return Next(Step7_Work);
                }
                if (Flow._stress >= GlobalConfig.StressMax)
                {
                    Describe("too stressed -> sleep");
                    return Next(Step5_Sleep);
                }
                Describe("need more ability -> train");
                return Next(Step3_Train);
            }

            private StepResult Step3_Train()
            {
                Describe("training (+1 ability, 3h)");
                // Offload the blocking 3h to the pool; carry the AsyncId to the wait.
                _job = SubmitAsync(() => (object)SimWork.SimHours(3), "sim_hours");
                if (_job == 0)
                {
                    return Fail();   // in-flight cap reached
                }
                return Next(Step4_TrainWait);
            }

            private StepResult Step4_TrainWait()
            {
                // Poll the submission by id. While in flight, keep polling this step.
                AsyncOutcome<int> r = AsyncResult<int>(_job);
                if (r.Pending)
                {
                    Describe("training...");
                    return Stay();
                }
                if (!r.Ok)
                {
                    return Fail();
                }
                Flow._ability += 1;
                Flow._stress += 1;
                Flow._hoursSpent += r.ReturnValue;
                GlobalLog.Add("student trained -> ability=" + Flow._ability + " stress=" + Flow._stress);
                Describe("trained: ability=" + Flow._ability + " stress=" + Flow._stress);
                return Next(Step2_CheckAbility);
            }

            private StepResult Step5_Sleep()
            {
                Describe("sleeping 6h");
                _job = SubmitAsync(() => (object)SimWork.SimHours(6), "sim_hours");
                if (_job == 0)
                {
                    return Fail();
                }
                return Next(Step6_SleepWait);
            }

            private StepResult Step6_SleepWait()
            {
                AsyncOutcome<int> r = AsyncResult<int>(_job);
                if (r.Pending)
                {
                    Describe("sleeping...");
                    return Stay();
                }
                if (!r.Ok)
                {
                    return Fail();
                }
                Flow._stress = Math.Max(0, Flow._stress - 6);
                Flow._hoursSpent += r.ReturnValue;
                GlobalLog.Add("student slept -> stress=" + Flow._stress);
                Describe("slept: stress=" + Flow._stress);
                return Next(Step2_CheckAbility);
            }

            private StepResult Step7_Work()
            {
                Describe("doing \"" + Flow._current.Name + "\" for " + Flow._current.NeedTime + "h");
                _job = SubmitAsync(() => (object)SimWork.SimHours(Flow._current.NeedTime), "sim_hours");
                if (_job == 0)
                {
                    return Fail();
                }
                return Next(Step8_WorkWait);
            }

            private StepResult Step8_WorkWait()
            {
                AsyncOutcome<int> r = AsyncResult<int>(_job);
                if (r.Pending)
                {
                    Describe("working...");
                    return Stay();
                }
                if (!r.Ok)
                {
                    return Fail();
                }
                Flow._hoursSpent += r.ReturnValue;
                Flow._stress += 1;
                Flow._doneCount += 1;
                GlobalLog.Add("student FINISHED assignment \"" + Flow._current.Name
                              + "\"  (stress=" + Flow._stress + ", done=" + Flow._doneCount + ")");
                Describe("finished \"" + Flow._current.Name + "\"");
                return Next(Step1_TakeNext);
            }

            // --- play chain: play burns off stress ---
            private StepResult Step9_Play()
            {
                Describe("playing \"" + Flow._current.Name + "\" " + Flow._current.PlayHours + "h");
                _job = SubmitAsync(() => (object)SimWork.SimHours(Flow._current.PlayHours), "sim_hours");
                if (_job == 0)
                {
                    return Fail();
                }
                return Next(Step10_PlayWait);
            }

            private StepResult Step10_PlayWait()
            {
                AsyncOutcome<int> r = AsyncResult<int>(_job);
                if (r.Pending)
                {
                    Describe("playing...");
                    return Stay();
                }
                if (!r.Ok)
                {
                    return Fail();
                }
                int relief = r.ReturnValue / 3;
                Flow._stress = Math.Max(0, Flow._stress - relief);
                Flow._hoursSpent += r.ReturnValue;
                Flow._doneCount += 1;
                GlobalLog.Add("student played \"" + Flow._current.Name + "\" -> stress=" + Flow._stress);
                Describe("played \"" + Flow._current.Name + "\"  stress=" + Flow._stress);
                return Next(Step1_TakeNext);
            }
        }
    }

    // ========================================================================
    // Flow_Visualization - snapshot the world into Snapshot.Latest each round.
    // ========================================================================
    //
    // A renderer feed is just another flow on the same pump - it reads every peer
    // with plain member access (no lock) because they share the thread. The actual
    // drawing runs on the background render thread (Renderer.Run).
    public sealed class Flow_Visualization : Module
    {
        public readonly App App;
        public readonly Task_Snapshot CtxSnapshot;

        public Flow_Visualization(Runtime rt, App app) : base(rt, "Flow_Visualization")
        {
            App = app;
            CtxSnapshot = new Task_Snapshot();
            AddTask(CtxSnapshot);
        }

        public sealed class Task_Snapshot : Task<Flow_Visualization>
        {
            protected override StepResult Entry() => Step1_Tick();

            private StepResult Step1_Tick()
            {
                if (GlobalEnv.Stop())
                {
                    Describe("viz stop");
                    return Done();
                }

                App app = Flow.App;
                Flow_Professor prof = app.Prof;
                Flow_Friend frnd = app.Friend;
                Flow_Student stu = app.Student;

                var s = new Snapshot
                {
                    ProfEmitted = prof.Emitted,
                    ProfTotal = prof.Total,
                    ProfIdle = prof.IsIdle,
                    ProfPhase = prof.CurrentStepDescription,

                    FriendEmitted = frnd.Emitted,
                    FriendTotal = frnd.Total,
                    FriendIdle = frnd.IsIdle,
                    FriendPhase = frnd.CurrentStepDescription,

                    StudentCurrent = stu.CurrentMessage,
                    StudentHasMsg = !stu.IsIdle,
                    StudentAbility = stu.Ability,
                    StudentStress = stu.Stress,
                    StudentHours = stu.HoursSpent,
                    StudentDone = stu.DoneCount,
                    StudentIdle = stu.IsIdle,
                    StudentPhase = stu.CurrentStepDescription,
                };

                // Walk the mailbox without popping - safe, same pump thread.
                foreach (Message m in Mailbox.Items())
                {
                    s.Queue.Add(m);
                }

                s.Recent = GlobalLog.Recent(6);

                Snapshot.Publish(s);
                return Stay();   // re-poll every round to keep it fresh
            }
        }
    }
}
