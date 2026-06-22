// Program.cs - render thread paints an ANSI dashboard; main thread blocks on Enter.
//
// C# port of main.cpp + the render half of uf_visualization.cpp. A background
// render thread repaints the dashboard ~25 fps from the published Snapshot while
// the main thread blocks on Console.ReadLine() (Enter or stdin-EOF quits). The
// renderer OWNS stdout, so the Runtime uses a SILENT observer and all activity
// goes into the GlobalLog ring shown on the dashboard - never printed directly.

#nullable enable

using System;
using System.Collections.Generic;
using System.Text;
using System.Threading;
using Uniflow;
using Uniflow.Examples;

namespace Uniflow.Examples.MessageDispatch
{
    public static class Program
    {
        private static readonly string Sep = "  " + new string('-', 70);

        private static string FmtMsg(Message m)
        {
            if (m.Kind == MessageKind.Assignment)
            {
                return "ASN  " + m.Name.PadRight(10) + " need_ability=" + m.NeedAbility
                       + " need_time=" + m.NeedTime + "h";
            }
            return "PLAY " + m.Name.PadRight(10) + " " + m.PlayHours + "h";
        }

        private static string MsgColor(Message m)
        {
            return m.Kind == MessageKind.Assignment ? Ansi.Yellow : Ansi.Green;
        }

        private static void Render(Snapshot s)
        {
            var lines = new List<string>();
            void Put(string text) => lines.Add(text);

            Put("  " + Ansi.Bold + "uniflow message_dispatch  " + Ansi.Reset
                + Ansi.Dim + "v" + Info.Version + Ansi.Reset);
            Put("  " + Ansi.Dim
                + "professor + friend -> shared mailbox -> student   "
                + "(one Runtime, one pump, lock-free)" + Ansi.Reset);
            Put(Sep);

            // ---- spawners ----
            bool profDone = s.ProfTotal > 0 && s.ProfIdle && s.ProfEmitted >= s.ProfTotal;
            Put("  " + Ansi.Yellow + "Professor" + Ansi.Reset
                + "  " + s.ProfEmitted + "/" + s.ProfTotal + " sent"
                + (profDone ? "  (done)" : "")
                + "   " + Ansi.Dim + s.ProfPhase + Ansi.Reset);

            bool friendDone = s.FriendTotal > 0 && s.FriendIdle && s.FriendEmitted >= s.FriendTotal;
            Put("  " + Ansi.Green + "Friend   " + Ansi.Reset
                + "  " + s.FriendEmitted + "/" + s.FriendTotal + " sent"
                + (friendDone ? "  (done)" : "")
                + "   " + Ansi.Dim + s.FriendPhase + Ansi.Reset);
            Put(Sep);

            // ---- mailbox ----
            Put("  " + Ansi.Bold + "Mailbox" + Ansi.Reset + "  (" + s.Queue.Count + " queued)");
            if (s.Queue.Count == 0)
            {
                Put("    " + Ansi.Dim
                    + "(empty - student resting or spawners between bursts)" + Ansi.Reset);
            }
            else
            {
                const int kMaxShown = 8;
                for (int i = 0; i < s.Queue.Count && i < kMaxShown; i++)
                {
                    Message m = s.Queue[i];
                    Put("    " + MsgColor(m) + FmtMsg(m) + Ansi.Reset);
                }
                if (s.Queue.Count > kMaxShown)
                {
                    Put("    " + Ansi.Dim + "... and " + (s.Queue.Count - kMaxShown) + " more" + Ansi.Reset);
                }
            }
            // Pad the mailbox region to a fixed floor so the rows below do not jump.
            int target = 7 + 9;
            while (lines.Count < target)
            {
                Put("");
            }
            Put(Sep);

            // ---- student ----
            string head = s.StudentIdle
                ? Ansi.Dim + "idle" + Ansi.Reset
                : "active: " + MsgColor(s.StudentCurrent) + FmtMsg(s.StudentCurrent) + Ansi.Reset;
            Put("  " + Ansi.Bold + "Student" + Ansi.Reset + "   " + head);
            Put("    " + Ansi.Dim + "phase: " + s.StudentPhase + Ansi.Reset);

            Put("    ability [" + Ansi.Cyan + Ansi.Bar(s.StudentAbility / 10.0, 20) + Ansi.Reset
                + "] " + s.StudentAbility.ToString().PadLeft(2) + "/10");

            string sc = s.StudentStress >= GlobalConfig.StressMax - 1 ? Ansi.Red
                      : s.StudentStress >= GlobalConfig.StressMax / 2 ? Ansi.Yellow
                      : Ansi.Green;
            Put("    stress  [" + sc + Ansi.Bar((double)s.StudentStress / GlobalConfig.StressMax, 20)
                + Ansi.Reset + "] " + s.StudentStress.ToString().PadLeft(2) + "/" + GlobalConfig.StressMax);

            Put("    hours spent: " + s.StudentHours + "        messages handled: " + s.StudentDone);
            Put(Sep);

            // ---- recent activity ----
            Put("  " + Ansi.Bold + "Recent activity" + Ansi.Reset);
            const int kLines = 6;
            int shown = 0;
            foreach (string line in s.Recent)
            {
                Put("    " + Ansi.Gray + line + Ansi.Reset);
                shown++;
            }
            while (shown < kLines)
            {
                Put("");
                shown++;
            }
            Put(Sep);
            Put("  " + Ansi.Dim + "press Enter to quit" + Ansi.Reset);

            // Compose: cursor save, paint each row at a fixed line with a clear, then restore.
            var sb = new StringBuilder();
            sb.Append(Ansi.SaveCursor);
            for (int i = 0; i < lines.Count; i++)
            {
                sb.Append(Ansi.At(i + 1, 1)).Append(Ansi.ClearLine).Append(lines[i]);
            }
            sb.Append(Ansi.RestoreCursor);
            Console.Out.Write(sb.ToString());
            Console.Out.Flush();
        }

        private static void RunRenderer(object? stopBox)
        {
            var stop = (ManualResetEventSlim)stopBox!;
            while (!stop.IsSet)
            {
                Render(Snapshot.Read());
                stop.Wait(40);   // ~25 fps, but returns early on quit
            }
        }

        public static int Main()
        {
            Ansi.EnableAnsi();
            Ansi.HideCursor();
            Ansi.Clear();

            var app = new App();
            app.Start();

            var stop = new ManualResetEventSlim(false);
            var renderer = new Thread(RunRenderer) { IsBackground = true, Name = "renderer" };
            renderer.Start(stop);

            try
            {
                // Enter (or EOF, e.g. piped input ended) quits. ReadLine returns null at EOF.
                Console.ReadLine();
            }
            finally
            {
                stop.Set();
                renderer.Join(TimeSpan.FromSeconds(1.0));
                app.Shutdown();
                Ansi.ShowCursor();
                Flow_Student stu = app.Student;
                Console.Out.Write(Ansi.At(40, 1) + Ansi.ClearLine
                    + "message_dispatch stopped:  hours=" + stu.HoursSpent
                    + "  ability=" + stu.Ability
                    + "  stress=" + stu.Stress
                    + "  messages_handled=" + stu.DoneCount + "\n");
                Console.Out.Flush();
            }
            return 0;
        }
    }
}
