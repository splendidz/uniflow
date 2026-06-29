// Program.cs - App (Runtime + every module) plus the render side and main.
//
// C# port of cpp/examples/queue_drain/ (app.h, main.cpp, uf_visualization.cpp
// render side) and python/examples/queue_drain.py.
//
// REFERENCE NOTE: one Runtime, one pump thread, three flows (sender, receiver,
// renderer). They cooperate without a single lock on the mailbox between them -
// the only mutex in the demo guards the render-thread snapshot. SILENT observer:
// this app OWNS the console (the dashboard), so the framework must print nothing.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Text;
using System.Threading;
using Uniflow;

namespace Uniflow.Examples.QueueDrain
{
    // ----- App - the Runtime plus every module (app.h). Two-phase init. -----
    public sealed class App
    {
        public readonly Runtime Rt;
        public readonly Flow_Sender Send;
        public readonly Flow_Receiver Recv;
        public readonly Flow_Visualization Viz;

        public App()
        {
            // Silent runtime (empty Observer prints nothing). Sleep knobs (seconds):
            // burst on any-Next round, short stay nap, tiny idle nap.
            Config cfg = new Config
            {
                IdleSleepSec = 0.001,
                StaySleepSec = 0.02,
                StepIntervalSleepSec = 0.0,
            };
            Rt = new Runtime(new Runtime.Options { Observer = new Observer(), Config = cfg });

            // Phase 1: construct every module.
            Send = new Flow_Sender(Rt);
            Recv = new Flow_Receiver(Rt);
            Viz = new Flow_Visualization(Rt);

            // Cross-module wiring (safe now that all modules exist).
            Send.Recv = Recv;
            Viz.Send = Send;
            Viz.Recv = Recv;
        }

        public void Start()
        {
            // Phase 2: launch the perpetual tasks. The receiver is NOT started here -
            // the sender relaunches its drain task on the first burst.
            Viz.TaskSnapshot.StartFlow();
            Send.TaskEmit.StartFlow();
        }

        public void Shutdown()
        {
            GlobalEnv.RequestStop();   // every step polls this and returns Done()
            Rt.Wake();                 // nudge the pump out of any sleep
            Send.WaitUntilIdle();
            Recv.WaitUntilIdle();
            Viz.WaitUntilIdle();
            Rt.Stop();
        }
    }

    // ----- render side - background-thread ANSI dashboard -----
    public static class Renderer
    {
        private static readonly string Sep = "  " + new string('-', 60);
        private const int KStatusRow = 15;

        private static string FormatMsg(Msg m)
        {
            return m.A + " " + m.Op + " " + m.B;
        }

        private static string StateColor(string state)
        {
            if (state == RecvState.Adding)
            {
                return Ansi.Fg(90, 180, 120);
            }
            if (state == RecvState.Subtracting)
            {
                return Ansi.Fg(210, 130, 60);
            }
            if (state == RecvState.Dispatching)
            {
                return Ansi.Fg(200, 190, 80);
            }
            if (state == RecvState.Idle)
            {
                return Ansi.Fg(120, 124, 134);
            }
            return Ansi.Reset;
        }

        private static void DrawConsole(Snapshot s)
        {
            StringBuilder outb = new StringBuilder();

            void Put(int row, string text)
            {
                outb.Append(Ansi.At(row, 1)).Append(Ansi.ClearLine).Append(text);
            }

            Put(1, "  " + Ansi.Bold + "uniflow queue_drain  " + Ansi.Reset
                   + Ansi.Dim + "v" + Info.Version + Ansi.Reset);
            Put(2, Sep);

            // sender line: burst counters + current step description.
            Put(3, "  " + Ansi.Cyan + "sender" + Ansi.Reset
                   + "   bursts " + Ansi.Bold + s.TotalBursts + "/"
                   + GlobalConfig.KMaxBurstCount + Ansi.Reset
                   + "   last burst " + s.LastBurstCount
                   + "   " + Ansi.Dim + s.SenderPhase + Ansi.Reset);

            // source vectors the sender draws operands from.
            StringBuilder a = new StringBuilder("  vec A:");
            StringBuilder b = new StringBuilder("  vec B:");
            foreach (int v in s.VecA)
            {
                a.Append(' ').Append(v.ToString().PadLeft(2));
            }
            foreach (int v in s.VecB)
            {
                b.Append(' ').Append(v.ToString().PadLeft(2));
            }
            Put(4, Ansi.Gray + a + Ansi.Reset);
            Put(5, Ansi.Gray + b + Ansi.Reset);

            Put(6, Sep);

            // queue depth + chip view of the pending jobs (lock-free list snapshot).
            Put(7, "  " + Ansi.Yellow + "queue" + Ansi.Reset
                   + "    depth " + Ansi.Bold + s.Queue.Count + Ansi.Reset);

            StringBuilder chips = new StringBuilder("  ");
            int shown = 0;
            while (shown < s.Queue.Count && shown < 12)
            {
                chips.Append('[').Append(FormatMsg(s.Queue[shown])).Append("] ");
                shown++;
            }
            if (s.Queue.Count > shown)
            {
                chips.Append('+').Append(s.Queue.Count - shown).Append(" more");
            }
            if (s.Queue.Count == 0)
            {
                chips.Append(Ansi.Dim).Append("(empty)").Append(Ansi.Reset);
            }
            Put(8, chips.ToString());

            Put(9, Sep);

            // receiver line: state chip + processed count + current step description.
            Put(10, "  " + StateColor(s.RecvState) + "receiver " + s.RecvState
                    + Ansi.Reset + "   processed " + Ansi.Bold + s.Processed
                    + Ansi.Reset + "   " + Ansi.Dim + s.RecvPhase + Ansi.Reset);
            Put(11, "  last result: " + Ansi.Bold
                    + (string.IsNullOrEmpty(s.LastResult) ? "-" : s.LastResult) + Ansi.Reset);

            // cycle speed: how fast the receiver drains one job at a time.
            Put(12, "  " + Ansi.Cyan + "cycle" + Ansi.Reset
                    + "    last " + Ansi.Bold
                    + s.LastCycleMs.ToString("0.0", CultureInfo.InvariantCulture)
                    + " ms/job" + Ansi.Reset
                    + "   " + s.JobsPerSec.ToString("0.0", CultureInfo.InvariantCulture)
                    + " jobs/s"
                    + "   " + Ansi.Dim + "avg "
                    + s.AvgMsPerJob.ToString("0.0", CultureInfo.InvariantCulture)
                    + " ms/job" + Ansi.Reset);

            Put(13, Sep);
            Put(14, "  " + Ansi.Dim + "press Enter to quit" + Ansi.Reset);

            Console.Write(outb.ToString());
            Console.Out.Flush();
        }

        public static void Run()
        {
            Ansi.EnableAnsi();
            Ansi.HideCursor();
            Ansi.Clear();

            // Render on a background thread; the main thread blocks on stdin so a
            // single Enter (or EOF) quits. The render thread only READS the snapshot.
            bool done = false;
            Thread render = new Thread(() =>
            {
                while (!Volatile.Read(ref done) && !GlobalEnv.Stop)
                {
                    DrawConsole(SnapshotStore.Read());
                    Thread.Sleep(40);   // ~25 fps
                }
            })
            { Name = "qd-render", IsBackground = true };
            render.Start();

            Console.ReadLine();   // any Enter (or EOF) quits
            Volatile.Write(ref done, true);
            render.Join();

            Ansi.ShowCursor();
            Console.Write(Ansi.At(KStatusRow, 1) + Ansi.ClearLine
                          + "  queue_drain stopped.\n");
            Console.Out.Flush();
        }
    }

    public static class Program
    {
        public static int Main()
        {
            App app = new App();   // Phase 1: every module is now constructed.
            app.Start();           // Phase 2: flows start; cross-module refs safe.

            Renderer.Run();        // main-thread render loop (background draw + stdin).

            app.Shutdown();

            Snapshot s = SnapshotStore.Read();
            Console.WriteLine("  bursts sent: " + s.TotalBursts
                              + "   jobs processed: " + s.Processed);
            return 0;
        }
    }
}
