// Flows.cs - the three modules: Flow_Sender, Flow_Receiver, Flow_Visualization.
//
// C# port of cpp/examples/queue_drain/ (uf_sender.*, uf_receiver.*,
// uf_visualization.*) and python/examples/queue_drain.py.
//
// FEATURE FOCUS: park / relaunch wake. When the queue empties the receiver
// returns Done() and its module PARKS (goes idle). The sender, on its next
// burst, sees recv.IsIdle and relaunches the drain task with CtxDrain.StartFlow()
// - the classic IsIdle + StartFlow wake pattern. Both calls happen inline on the
// same pump thread, so they are plain in-thread calls: no lock, no signal.
using System;
using System.Collections.Generic;
using Uniflow;

namespace Uniflow.Examples.QueueDrain
{
    // ----- Flow_Sender - the feed module that stuffs the inbox in bursts -----
    //
    // One perpetual task (Emit): every KSendGap, push a random burst of 1..N jobs
    // into the mailbox, then relaunch the receiver if it has parked. Because both
    // modules run on the same pump thread, pushing to the mailbox and calling
    // recv.IsIdle / StartFlow inline are all lock-free, in-thread calls.
    public sealed class Flow_Sender : Module
    {
        private readonly Random _rng = new Random();

        // Flow-owned state, reached from the step via Flow.Member.
        public List<int> VecA = new List<int>();
        public List<int> VecB = new List<int>();
        public int LastBurstCount;
        public int TotalBursts;

        // The receiver, wired by the App after construction (relaunch target).
        public Flow_Receiver? Recv;

        public readonly Task_Emit CtxEmit;

        public Flow_Sender(Runtime rt) : base(rt, "Flow_Sender")
        {
            FillVectors();
            CtxEmit = new Task_Emit();
            AddTask(CtxEmit);
        }

        private void FillVectors()
        {
            for (int i = 0; i < GlobalConfig.KVecSize; i++)
            {
                VecA.Add(_rng.Next(1, GlobalConfig.KValueMax + 1));
                VecB.Add(_rng.Next(1, GlobalConfig.KValueMax + 1));
            }
        }

        private int EmitBurst()
        {
            int n = _rng.Next(GlobalConfig.KBurstMin, GlobalConfig.KBurstMax + 1);
            LastBurstCount = n;
            TotalBursts += 1;
            for (int i = 0; i < n; i++)
            {
                Msg m = new Msg
                {
                    A = VecA[_rng.Next(0, GlobalConfig.KVecSize)],
                    B = VecB[_rng.Next(0, GlobalConfig.KVecSize)],
                    Op = _rng.Next(0, 2) == 0 ? "+" : "-",
                };
                Mailbox.Push(m);   // lock-free: only this pump thread touches the queue
            }
            return n;
        }

        public sealed class Task_Emit : Task<Flow_Sender>
        {
            private UFTimer _gap = new UFTimer();

            protected override void OnEnter()
            {
                // Re-arm the burst timer on task entry (real wall clock).
                _gap = new UFTimer();
                _gap.Restart();
            }

            protected override StepResult Entry() => Step1_Tick();

            private StepResult Step1_Tick()
            {
                if (GlobalEnv.Stop)
                {
                    Describe("stop requested -> done");
                    return Done();
                }
                if (Flow.TotalBursts >= GlobalConfig.KMaxBurstCount)
                {
                    // Burst budget spent: stop emitting so the demo settles. The
                    // receiver drains the final burst and parks; dashboard runs on.
                    Describe("burst budget exhausted -> done");
                    return Done();
                }

                // Throttle on wall time. _gap was armed by OnEnter and survives Stay
                // re-entries, so Passed() measures from task entry / last Restart.
                if (!_gap.Passed(GlobalConfig.KSendGap))
                {
                    Describe("idle gap");
                    return Stay();   // re-poll this step next round
                }

                int n = Flow.EmitBurst();
                _gap.Restart();
                Describe("burst pushed: " + n + " jobs (queue=" + Mailbox.Size + ")");

                // Wake the receiver if it has parked. Same pump thread, so IsIdle
                // and StartFlow are plain in-thread calls - no lock, no signal.
                if (Flow.Recv!.IsIdle)
                {
                    Flow.Recv!.CtxDrain.StartFlow();
                }

                return Stay();
            }
        }
    }

    // ----- Flow_Receiver - the worker module that drains the inbox -----
    //
    // One looping task (Drain): pop one Msg, dispatch to the Add or Sub step by
    // its operator, then loop back to pop the next. When the queue empties the
    // task Done()s and the module PARKS; the sender relaunches it on the next
    // burst. State the steps share (current job, counters) lives on the flow.
    public sealed class Flow_Receiver : Module
    {
        // Flow-owned state, reached from the steps via Flow.Member.
        public string State = RecvState.Idle;
        public Msg Current = new Msg();
        public int Processed;
        public string LastResult = "";

        public readonly Task_Drain CtxDrain;

        public Flow_Receiver(Runtime rt) : base(rt, "Flow_Receiver")
        {
            CtxDrain = new Task_Drain();
            AddTask(CtxDrain);
        }

        public sealed class Task_Drain : Task<Flow_Receiver>
        {
            protected override StepResult Entry() => Step1_TakeNext();

            private StepResult Step1_TakeNext()
            {
                if (GlobalEnv.Stop)
                {
                    return Done();
                }
                Flow.State = RecvState.Dispatching;

                // Pop one job. Mailbox is touched only on this pump thread, no lock.
                Msg? m = Mailbox.TryPop();
                if (m == null)
                {
                    // Queue drained: park the module. Done() lets it idle until the
                    // sender relaunches this task (StartFlow) on the next burst.
                    Flow.State = RecvState.Idle;
                    Describe("queue drained -> done");
                    return Done();
                }
                Flow.Current = m;
                Describe("popped " + m.A + " " + m.Op + " " + m.B);

                // Dispatch by operator: Next() routes to a sibling step in this task.
                if (m.Op == "+")
                {
                    return Next(Step2_Add);
                }
                return Next(Step3_Sub);
            }

            private StepResult Step2_Add()
            {
                Flow.State = RecvState.Adding;
                int result = Flow.Current.A + Flow.Current.B;
                Flow.LastResult = Flow.Current.A + " + " + Flow.Current.B + " = " + result;
                Flow.Processed += 1;
                Describe("add: " + Flow.LastResult);
                return Next(Step1_TakeNext);   // loop back for the next job
            }

            private StepResult Step3_Sub()
            {
                Flow.State = RecvState.Subtracting;
                int result = Flow.Current.A - Flow.Current.B;
                Flow.LastResult = Flow.Current.A + " - " + Flow.Current.B + " = " + result;
                Flow.Processed += 1;
                Describe("sub: " + Flow.LastResult);
                return Next(Step1_TakeNext);
            }
        }
    }

    // ----- Flow_Visualization - pump-side snapshot writer -----
    //
    // A module on the pump thread whose one perpetual step copies live sender /
    // receiver / mailbox state into the snapshot each tick (under the snapshot
    // mutex), so the render thread always sees a consistent frame. Perpetual poll
    // - it ends only on stop.
    public sealed class Flow_Visualization : Module
    {
        // Wired by the App after construction (read-only, same pump thread).
        public Flow_Sender? Send;
        public Flow_Receiver? Recv;

        public readonly Task_Snapshot CtxSnapshot;

        public Flow_Visualization(Runtime rt) : base(rt, "Flow_Visualization")
        {
            CtxSnapshot = new Task_Snapshot();
            AddTask(CtxSnapshot);
        }

        public sealed class Task_Snapshot : Task<Flow_Visualization>
        {
            // Per-job cycle-speed timing state (real wall clock, pump thread only).
            private bool _started;
            private long _start;            // Stopwatch ticks at first tick
            private long _lastJob;          // Stopwatch ticks at last completion
            private int _lastProcessed;
            private double _lastCycleMs;

            protected override StepResult Entry() => Step1_Tick();

            private StepResult Step1_Tick()
            {
                if (GlobalEnv.Stop)
                {
                    return Done();
                }
                Flow_Sender s = Flow.Send!;
                Flow_Receiver r = Flow.Recv!;

                // Measure the per-job cycle speed: how long the receiver takes
                // between consecutive job completions, plus the overall average.
                long now = System.Diagnostics.Stopwatch.GetTimestamp();
                if (!_started)
                {
                    _start = now;
                    _lastJob = now;
                    _started = true;
                }
                double toMs = 1000.0 / System.Diagnostics.Stopwatch.Frequency;
                int processed = r.Processed;
                if (processed > _lastProcessed)
                {
                    double dt = (now - _lastJob) * toMs;
                    _lastCycleMs = dt / (processed - _lastProcessed);
                    _lastJob = now;
                    _lastProcessed = processed;
                }
                double elapsedMs = (now - _start) * toMs;
                double avgMs = processed > 0 ? elapsedMs / processed : 0.0;
                double jps = _lastCycleMs > 0.0 ? 1000.0 / _lastCycleMs : 0.0;

                SnapshotStore.Write(g =>
                {
                    g.LastCycleMs = _lastCycleMs;
                    g.JobsPerSec = jps;
                    g.AvgMsPerJob = avgMs;
                    g.VecA = new List<int>(s.VecA);
                    g.VecB = new List<int>(s.VecB);
                    g.Queue = Mailbox.Snapshot();
                    g.LastBurstCount = s.LastBurstCount;
                    g.TotalBursts = s.TotalBursts;
                    g.RecvState = r.State;
                    g.Processed = r.Processed;
                    g.LastResult = r.LastResult;
                    g.Current = r.Current;
                    g.SenderPhase = s.CurrentStepDescription;
                    g.RecvPhase = r.CurrentStepDescription;
                });
                return Stay();
            }
        }
    }
}
