// Program.cs - one-pump-thread CNC pick-and-place line (CONSOLE renderer).
//
// C# port of cpp/examples/pick_and_place/ (console back-end only; the Win32 GDI
// window is dropped) and python/examples/pick_and_place.py against the same
// uniflow framework. A pick-and-place line runs on ONE Runtime pump thread:
//
//   - Flow_LoadPicker   moves a raw part   zone A -> zone B  (Pick then Place)
//   - Flow_Stage        machines it at B   (Prepare -> Process -> Cleanup)
//   - Flow_UnloadPicker moves the finished part  zone B -> zone C  (Pick, Place)
//   - Flow_Orchestrator schedules every module's next task by state; the pickers
//     and the stage never sequence themselves.
//
// The ANSI console renderer is a side view drawn on a background thread ~20 fps;
// the main thread blocks on Console.ReadLine() (Enter / EOF quits). The runtime
// uses a SILENT observer because the renderer owns stdout.
//
// NOTE ON UNITS: the C++ port measures durations in milliseconds; this C#
// VirtualClock / UFTimer report SECONDS (like the Python port), so the same waits
// are written in seconds (process 5.0 s, ack timeout 2.0 s, hw settle 0.05 s).

#nullable enable

using System;
using System.Text;
using System.Threading;
using Uniflow;
using Uniflow.Examples;

namespace Uniflow.PickAndPlaceExample
{
    // ========================================================================
    // App - holds the Runtime + every module. SILENT observer: the ANSI renderer
    // owns stdout, so the framework must not print events.
    // ========================================================================
    internal sealed class App
    {
        private static App? _inst;

        public static App Inst()
        {
            return _inst ??= new App();
        }

        public readonly Runtime Rt;
        public readonly Flow_Stage Stage;
        public readonly Flow_Visualization Viz;
        public readonly Flow_LoadPicker Load;
        public readonly Flow_UnloadPicker Unload;
        public readonly Flow_Orchestrator Orch;

        public App()
        {
            // SILENT observer (a plain 'new Observer()' is the no-op observer).
            // All-Stay rounds (every module polling) should be short so motion and
            // the ~20 fps renderer stay smooth (sleeps are in SECONDS).
            var cfg = new Config
            {
                IdleSleepSec = 0.001,
                StaySleepSec = 0.005,
                StepIntervalSleepSec = 0.0,
            };
            Rt = new Runtime(new Runtime.Options { Observer = new Observer(), Config = cfg });
            Stage = new Flow_Stage(Rt);
            Viz = new Flow_Visualization(Rt);
            Load = new Flow_LoadPicker(Rt);
            Unload = new Flow_UnloadPicker(Rt);
            Orch = new Flow_Orchestrator(Rt);
        }

        public void Start()
        {
            Viz.TaskSnapshot.StartFlow();
            Orch.TaskSchedule.StartFlow();
        }

        public void Shutdown()
        {
            Env.RequestStop();
            Rt.Wake();
            Orch.WaitUntilIdle(2.0);
            Load.WaitUntilIdle(2.0);
            Unload.WaitUntilIdle(2.0);
            Stage.WaitUntilIdle(2.0);
            Viz.WaitUntilIdle(2.0);
            Rt.Stop();
        }
    }

    // ========================================================================
    // Console renderer - ANSI side view. A top rail the two pickers hang from,
    // arms descending by Z, zones A/B/C + stage table on the track below. Mirrors
    // uf_visualization_console.cpp. Render on a background thread ~20 fps; the
    // main thread blocks on input (Enter / EOF quits).
    // ========================================================================
    internal static class ConsoleRenderer
    {
        private const int KCols = 70;
        private const int KMargin = 4;
        private const int KRailRow = 0;
        private const int KZRows = 6;
        private const int KTrackRow = KRailRow + KZRows + 2;
        private const int KRows = KTrackRow + 2;
        private const int KHeaderRows = 3;

        private struct Cell
        {
            public char Ch;
            public string Color;
        }

        private static int ColX(double xMm)
        {
            double f = xMm / Geometry.X_MAX_MM;
            if (f < 0.0) f = 0.0;
            if (f > 1.0) f = 1.0;
            return KMargin + (int)(f * (KCols - 2 * KMargin) + 0.5);
        }

        private static void Put(Cell[] cv, int r, int c, char ch, string col)
        {
            if (r < 0 || r >= KRows || c < 0 || c >= KCols) return;
            cv[r * KCols + c].Ch = ch;
            cv[r * KCols + c].Color = col;
        }

        // Draw one picker: an arm of '|' from the rail down to its Z depth, with a
        // gripper tip ('#' carrying a part, 'v' open) in the picker's colour.
        private static void DrawPicker(Cell[] cv, double xMm, double zMm, bool carry, string col)
        {
            int c = ColX(xMm);
            double f = zMm / Geometry.Z_DOWN_MM;   // 0 up .. 1 fully down
            if (f < 0.0) f = 0.0;
            if (f > 1.0) f = 1.0;
            int tip = KRailRow + (int)(f * KZRows + 0.5);
            for (int r = KRailRow; r < tip; r++)
            {
                Put(cv, r, c, '|', col);
            }
            Put(cv, tip, c, carry ? '#' : 'v', col);
        }

        private static void DrawConsole(Snapshot s)
        {
            var cv = new Cell[KRows * KCols];
            for (int i = 0; i < cv.Length; i++)
            {
                cv[i].Ch = ' ';
                cv[i].Color = "";
            }

            string ldCol = Ansi.Fg(90, 150, 230);   // load picker: blue
            string ulCol = Ansi.Fg(230, 130, 90);   // unload picker: orange

            // rail across the top
            for (int c = KMargin; c < KCols - KMargin; c++)
            {
                Put(cv, KRailRow, c, '=', Ansi.Gray);
            }

            // track across the bottom, with zone markers
            for (int c = KMargin; c < KCols - KMargin; c++)
            {
                Put(cv, KTrackRow, c, '=', Ansi.Gray);
            }
            string markCol = Ansi.Bold + Ansi.Fg(225, 230, 240);
            Put(cv, KTrackRow, ColX(Geometry.ZONE_A_MM), 'A', markCol);
            Put(cv, KTrackRow, ColX(Geometry.ZONE_B_MM), 'B', markCol);
            Put(cv, KTrackRow, ColX(Geometry.ZONE_C_MM), 'C', markCol);

            // raw part waiting in zone A
            if (s.ZoneAHasPart)
            {
                Put(cv, KTrackRow - 1, ColX(Geometry.ZONE_A_MM), 'o', Ansi.Fg(210, 180, 70));
            }

            // stage table at B, coloured by machining state
            string stageCol;
            if (s.StageState == StageState.PROCESSED_PART_READY)
            {
                stageCol = Ansi.Fg(90, 180, 120);
            }
            else if (s.StageState == StageState.IDLE)
            {
                stageCol = Ansi.Fg(120, 124, 134);
            }
            else
            {
                stageCol = Ansi.Fg(210, 130, 60);
            }
            Put(cv, KTrackRow - 1, ColX(s.StageTableXMm), '#', stageCol);

            // the two pickers hanging from the rail
            DrawPicker(cv, s.LoadXMm, s.LoadZMm, s.LoadCarry, ldCol);
            DrawPicker(cv, s.UnloadXMm, s.UnloadZMm, s.UnloadCarry, ulCol);

            // compose one frame and write it in a single flush.
            var outBuf = new StringBuilder();
            outBuf.Append(Ansi.At(1, 1)).Append(Ansi.ClearLine).Append("  ").Append(Ansi.Bold)
                  .Append("uniflow pick & place  ").Append(Ansi.Reset).Append(Ansi.Dim)
                  .Append('v').Append(Info.Version).Append(Ansi.Reset);
            outBuf.Append(Ansi.At(2, 1)).Append(Ansi.ClearLine).Append("  ").Append("stage: ")
                  .Append(Ansi.Cyan).Append(s.StageState).Append(Ansi.Reset).Append(" (")
                  .Append(s.StagePhase).Append(")   delivered: ").Append(Ansi.Bold)
                  .Append(s.Delivered).Append(Ansi.Reset).Append("   ").Append(ldCol)
                  .Append("LD").Append(Ansi.Reset).Append('/').Append(ulCol).Append("UL")
                  .Append(Ansi.Reset);
            outBuf.Append(Ansi.At(3, 1)).Append(Ansi.ClearLine).Append("  ").Append(Ansi.Dim)
                  .Append("press Enter to quit").Append(Ansi.Reset);

            for (int r = 0; r < KRows; r++)
            {
                outBuf.Append(Ansi.At(r + 1 + KHeaderRows, 1)).Append(Ansi.ClearLine);
                string cur = "";
                for (int c = 0; c < KCols; c++)
                {
                    Cell cell = cv[r * KCols + c];
                    if (cell.Color != cur)
                    {
                        outBuf.Append(cell.Color.Length == 0 ? Ansi.Reset : cell.Color);
                        cur = cell.Color;
                    }
                    outBuf.Append(cell.Ch);
                }
                outBuf.Append(Ansi.Reset);
            }

            Console.Out.Write(outBuf.ToString());
            Console.Out.Flush();
        }

        public static void Run()
        {
            Ansi.EnableAnsi();
            Ansi.HideCursor();
            Ansi.Clear();

            // Render on a background thread; the main thread blocks on stdin so a
            // single Enter (or EOF) quits. The render thread only READS the snapshot.
            var done = new ManualResetEventSlim(false);

            var render = new Thread(() =>
            {
                while (!done.IsSet && !Env.Stop())
                {
                    DrawConsole(SnapshotStore.Read());
                    Thread.Sleep(50);
                }
            })
            {
                Name = "pp-render",
                IsBackground = true,
            };
            render.Start();

            Console.In.ReadLine();   // any Enter (or EOF) quits
            done.Set();
            render.Join(TimeSpan.FromSeconds(1.0));

            Ansi.ShowCursor();
            Console.Out.Write(Ansi.At(KRows + KHeaderRows + 2, 1) + Ansi.ClearLine
                              + "  pick_and_place stopped.\n");
            Console.Out.Flush();
        }
    }

    // ========================================================================
    // Program - main entry. Mirrors main.cpp / main() in the Python port.
    // ========================================================================
    internal static class Program
    {
        private static int Main()
        {
            App app = App.Inst();
            app.Start();

            ConsoleRenderer.Run();

            app.Shutdown();

            Console.Out.Write("parts delivered to Unload: " + Env.DeliveredCount() + "\n");
            Console.Out.Flush();
            return 0;
        }
    }
}
