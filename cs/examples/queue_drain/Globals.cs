// Globals.cs - shared value type, tuning constants, mailbox, snapshot.
//
// C# port of cpp/examples/queue_drain/ (globals.*, mailbox.*, snapshot.*) and
// python/examples/queue_drain.py. The sender and the receiver run as two modules
// on ONE pump thread, so the mailbox between them needs no lock at all - a
// lock-free single-pump producer / consumer. The only cross-thread lock in the
// whole demo guards the render-thread snapshot (g_snap).
using System.Collections.Generic;
using System.Threading;

namespace Uniflow.Examples.QueueDrain
{
    // One arithmetic job: the sender pushes it, the receiver pops / evaluates it.
    public sealed class Msg
    {
        public int A;
        public int B;
        public string Op = "+";
    }

    // Demo tuning constants (durations in SECONDS, matching the Python port; the
    // C++ original uses milliseconds for the same values).
    public static class GlobalConfig
    {
        public const int KVecSize = 10;
        public const int KValueMax = 30;
        public const int KBurstMin = 1;
        public const int KBurstMax = 10;
        public const double KSendGap = 0.6;     // seconds between bursts (C++ 600ms)
        public const int KMaxBurstCount = 20;   // stop the sender here
    }

    // Receiver states (uf_receiver enum mirror).
    public static class RecvState
    {
        public const string Idle = "Idle";
        public const string Dispatching = "Dispatching";
        public const string Adding = "Adding";
        public const string Subtracting = "Subtracting";
    }

    // Process-wide stop latch. The stdin loop sets it; every step polls it and
    // returns Done(), which lets WaitUntilIdle() return at shutdown.
    public static class GlobalEnv
    {
        private static volatile bool _stop;
        public static bool Stop => _stop;
        public static void RequestStop() { _stop = true; }
    }

    // Mailbox - the FIFO the sender enqueues into and the receiver drains.
    //
    // Sender + receiver both live on the same Runtime, so the queue is touched
    // only by one pump thread - a plain list, no lock. The viz snapshot step also
    // runs on the pump thread, so Snapshot() is lock-free too.
    public static class Mailbox
    {
        private static readonly List<Msg> Queue = new List<Msg>(); // pump-thread-only

        public static void Push(Msg m)
        {
            Queue.Add(m);
        }

        // Returns the next Msg or null. Lock-free: only the pump thread touches it.
        public static Msg? TryPop()
        {
            if (Queue.Count == 0)
            {
                return null;
            }
            Msg m = Queue[0];
            Queue.RemoveAt(0);
            return m;
        }

        public static int Size => Queue.Count;

        // Copy the queued items for the visualisation. Called on the pump thread.
        public static List<Msg> Snapshot()
        {
            return new List<Msg>(Queue);
        }
    }

    // One frame of state for the renderer. The viz snapshot step (pump thread)
    // writes g_snap under g_snap_mu; the background render thread reads it under
    // the same mutex - the demo's ONLY cross-thread synchronisation.
    public sealed class Snapshot
    {
        public List<int> VecA = new List<int>();
        public List<int> VecB = new List<int>();
        public List<Msg> Queue = new List<Msg>();
        public int LastBurstCount;
        public int TotalBursts;

        // Per-job cycle speed: how fast the receiver drains one job at a time.
        public double LastCycleMs;    // wall time the most recent job took (ms)
        public double JobsPerSec;     // instantaneous rate = 1000 / LastCycleMs
        public double AvgMsPerJob;    // overall average = total elapsed ms / processed

        public string RecvState = QueueDrain.RecvState.Idle;
        public int Processed;
        public string LastResult = "";
        public Msg Current = new Msg();

        public string SenderPhase = "";
        public string RecvPhase = "";
    }

    public static class SnapshotStore
    {
        private static readonly object Mu = new object();
        private static Snapshot _snap = new Snapshot();

        // Write the live frame under the mutex (called on the pump thread).
        public static void Write(System.Action<Snapshot> fill)
        {
            lock (Mu)
            {
                fill(_snap);
            }
        }

        // By-value snapshot read under the mutex (render / main thread).
        public static Snapshot Read()
        {
            lock (Mu)
            {
                Snapshot s = new Snapshot
                {
                    VecA = new List<int>(_snap.VecA),
                    VecB = new List<int>(_snap.VecB),
                    Queue = new List<Msg>(_snap.Queue),
                    LastBurstCount = _snap.LastBurstCount,
                    TotalBursts = _snap.TotalBursts,
                    LastCycleMs = _snap.LastCycleMs,
                    JobsPerSec = _snap.JobsPerSec,
                    AvgMsPerJob = _snap.AvgMsPerJob,
                    RecvState = _snap.RecvState,
                    Processed = _snap.Processed,
                    LastResult = _snap.LastResult,
                    Current = _snap.Current,
                    SenderPhase = _snap.SenderPhase,
                    RecvPhase = _snap.RecvPhase,
                };
                return s;
            }
        }
    }
}
