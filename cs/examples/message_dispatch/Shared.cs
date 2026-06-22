// Shared.cs - shared types for message_dispatch: Message, Mailbox, GlobalLog,
// GlobalEnv (quit latch), Snapshot. C# port of globals.* / mailbox.* / snapshot.*.
//
// The two spawners push Messages into a shared Mailbox; the Student drains it one
// at a time and routes by Message.Kind. All modules live on ONE Runtime (one pump
// thread), so the mailbox needs NO lock - only the pump thread ever touches it.
// GlobalLog and Snapshot ARE locked: the background render thread reads them.

#nullable enable

using System.Collections.Generic;
using System.Threading;

namespace Uniflow.Examples.MessageDispatch
{
    // ----- Message: one mailbox item -----
    // Assignment uses NeedAbility / NeedTime; Play uses PlayHours.
    public enum MessageKind
    {
        Assignment,
        Play
    }

    public sealed class Message
    {
        public MessageKind Kind;
        public string Name = "";
        public int NeedAbility;   // Assignment: required ability
        public int NeedTime;      // Assignment: hours of actual work
        public int PlayHours;     // Play: hours of playing

        public Message()
        {
        }

        public Message(MessageKind kind, string name, int needAbility, int needTime, int playHours)
        {
            Kind = kind;
            Name = name;
            NeedAbility = needAbility;
            NeedTime = needTime;
            PlayHours = playHours;
        }
    }

    // ----- GlobalConfig: demo tuning constants -----
    public static class GlobalConfig
    {
        // One simulated hour on the pool (seconds). Keeps the demo finite.
        public const double HourSec = 0.008;
        // At or above this stress, the student must sleep before training.
        public const int StressMax = 10;

        // Spawner gaps (seconds).
        public const double ProfMinGap = 0.200;
        public const double ProfMaxGap = 0.800;
        public const double FriendMinGap = 0.350;
        public const double FriendMaxGap = 1.100;
    }

    // ----- GlobalEnv: quit latch -----
    // Main sets it after Enter/EOF; every step checks it and returns Done() so
    // WaitUntilIdle() can return.
    public static class GlobalEnv
    {
        private static int _stop;

        public static void SetStop()
        {
            Interlocked.Exchange(ref _stop, 1);
        }

        public static bool Stop()
        {
            return Volatile.Read(ref _stop) != 0;
        }
    }

    // ----- GlobalLog: recent-activity ring -----
    // Pump-thread steps append lines (a stray Console.Write would corrupt the ANSI
    // dashboard); the render thread reads the last few for the "recent activity"
    // panel - hence the lock.
    public static class GlobalLog
    {
        private const int Cap = 64;
        private static readonly object Mu = new object();
        private static readonly List<string> Lines = new List<string>();

        public static void Add(string line)
        {
            lock (Mu)
            {
                Lines.Add(line);
                if (Lines.Count > Cap)
                {
                    Lines.RemoveRange(0, Lines.Count - Cap);
                }
            }
        }

        public static List<string> Recent(int n)
        {
            lock (Mu)
            {
                int start = Lines.Count - n;
                if (start < 0)
                {
                    start = 0;
                }
                return Lines.GetRange(start, Lines.Count - start);
            }
        }
    }

    // ----- Mailbox: shared inbox for the student -----
    // The two spawners push; the student pops one at a time. All modules are on the
    // SAME Runtime, so the mailbox only ever sees the pump thread and needs no lock.
    public static class Mailbox
    {
        private static readonly List<Message> Inbox = new List<Message>();

        public static void Push(Message m)
        {
            Inbox.Add(m);
        }

        // Returns the front Message, or null if empty.
        public static Message? TryPop()
        {
            if (Inbox.Count == 0)
            {
                return null;
            }
            Message m = Inbox[0];
            Inbox.RemoveAt(0);
            return m;
        }

        public static int Size()
        {
            return Inbox.Count;
        }

        // Walk front-to-back without popping (viz snapshot, same pump thread).
        public static IReadOnlyList<Message> Items()
        {
            return Inbox;
        }
    }

    // ----- Snapshot: the data the renderer draws each frame -----
    // The viz task on the pump thread writes Latest under SnapMu every round; the
    // render thread reads a copy under the same lock. Cross-thread, so it carries a
    // lock (unlike the peer reads inside the pump, which need none).
    public sealed class Snapshot
    {
        public List<Message> Queue = new List<Message>();

        public int ProfEmitted;
        public int ProfTotal;
        public string ProfPhase = "";
        public bool ProfIdle = true;

        public int FriendEmitted;
        public int FriendTotal;
        public string FriendPhase = "";
        public bool FriendIdle = true;

        public Message StudentCurrent = new Message();
        public bool StudentHasMsg;
        public int StudentAbility;
        public int StudentStress;
        public int StudentHours;
        public int StudentDone;
        public string StudentPhase = "";
        public bool StudentIdle = true;

        public List<string> Recent = new List<string>();

        private static readonly object SnapMu = new object();
        private static Snapshot _latest = new Snapshot();

        public static void Publish(Snapshot s)
        {
            lock (SnapMu)
            {
                _latest = s;
            }
        }

        public static Snapshot Read()
        {
            lock (SnapMu)
            {
                return _latest;
            }
        }
    }
}
