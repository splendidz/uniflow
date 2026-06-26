// simulator - many flows share one pump and one logical clock.
//
// C# port of cpp/examples/simulator/ and python/examples/simulator.py against the
// same uniflow framework. A console sim where five runner flows plus a renderer
// flow all live on ONE Runtime (one pump thread) and one VirtualClock. Type
// commands to drive time itself:
//
//   pause       freeze logical time  (runners stop, dashboard stays live)
//   resume      resume logical time
//   speed <n>   scale logical time   (n > 0; 0.5 = half, 4 = 4x)
//   quit        stop and exit
//
// FEATURE FOCUS: VirtualClock (scale / freeze) + single-thread cooperation. The
// pump, the five runners, and the renderer all live on one thread; the main
// thread does nothing but read stdin and poke the clock (thread-safe).
//
// NOTE ON UNITS: the C++ port measures phases in milliseconds; this C#
// VirtualClock.Now() returns SECONDS (like the Python port), so the same
// durations are in seconds (move 3.8 s vs 3800 ms, gate 0.7 s, rest 0.5 s).

#nullable enable

using System;
using System.Text;
using Uniflow;
using Uniflow.Examples;

namespace Uniflow.SimulatorExample
{
    // ========================================================================
    // Snapshot - the data the renderer draws each frame (mirrors snapshot.h/.cpp)
    // ========================================================================
    //
    // REFERENCE NOTE: there is NO lock here, on purpose. Every Flow_Runner and the
    // Flow_View renderer are modules on the SAME Runtime, so they all advance on
    // the one pump thread, round-robin. A runner writing its row and the view
    // reading it never overlap - that is the core uniflow guarantee: single
    // thread, no locks. The only other thread (stdin in Main) touches just the
    // clock and the stop flag.

    // One dashboard line. The runner owns its slot (indexed by ctor id).
    internal sealed class RunnerRow
    {
        public string Name = "";
        public string Step = "-";   // current step name - the "what is it doing" column
        public double Percent = 0.0;
        public int Lap = 0;
    }

    // Plain process-level state, pump-thread-only (see note above).
    internal static class Sim
    {
        public const int RunnerCount = 5;

        // Fixed dashboard layout (1-based rows). The renderer only ever draws rows
        // 1..(PromptRow-1); the stdin prompt lives on PromptRow and is never
        // touched by the renderer, so typed input is not clobbered by redraws.
        public const int PromptRow = 6 + RunnerCount;

        // Plain shared array, written by runners and read by the view on the SAME
        // pump thread - lock-free single-thread snapshot.
        public static readonly RunnerRow[] Rows = NewRows();

        // Cross-thread shutdown latch: set by the stdin loop, read by every flow's
        // steps so they can return Done() and let WaitUntilIdle() return.
        public static volatile bool Stop = false;

        private static RunnerRow[] NewRows()
        {
            var rows = new RunnerRow[RunnerCount];
            for (int i = 0; i < RunnerCount; i++)
            {
                rows[i] = new RunnerRow();
            }
            return rows;
        }
    }

    // ========================================================================
    // Flow_Runner - one simulated worker that laps a track forever (uf_runner.*)
    // ========================================================================
    //
    // FEATURE FOCUS: logical (virtual) time. Progress is measured against
    // Flow.Clock (the Runtime's VirtualClock) - so every runner speeds up, slows
    // down, and freezes together when the user scales or pauses the sim, with no
    // per-flow plumbing. The three steps (Gate -> Move -> Rest) loop; each loop is
    // one lap.
    internal sealed class Flow_Runner : Module
    {
        public const double GateSec = 0.7;   // virtual-time hold at the gate
        public const double RestSec = 0.5;   // virtual-time rest after the line

        public readonly int Id;
        public readonly double MoveSec;
        public int Lap = 0;

        public readonly Task_Run CtxRun;

        // idx selects this runner's dashboard row; moveSec is the virtual-time
        // length of one Move phase (different per runner for a staggered field).
        public Flow_Runner(Runtime rt, string name, int idx, double moveSec)
            : base(rt, name)
        {
            Id = idx;
            MoveSec = moveSec;
            CtxRun = new Task_Run();
            AddTask(CtxRun);
            Sim.Rows[Id].Name = name;
        }

        public sealed class Task_Run : Task<Flow_Runner>
        {
            private double _phaseStart;

            // Re-anchor virtual time whenever the task is entered.
            protected override void OnEnter()
            {
                _phaseStart = Flow.Clock.Now();
            }

            protected override StepResult Entry()
            {
                return Step1_Gate();
            }

            // Virtual seconds elapsed in the current phase. Because Now() comes
            // from the VirtualClock, this stalls when the sim is paused and
            // stretches / compresses when it is scaled - the point of this example.
            private double VElapsedSec()
            {
                return Flow.Clock.Now() - _phaseStart;
            }

            private void Publish(double percent, string step)
            {
                // Plain writes to our own row - same pump thread as the renderer,
                // no lock.
                RunnerRow row = Sim.Rows[Flow.Id];
                row.Percent = percent;
                row.Step = step;
                row.Lap = Flow.Lap;
            }

            private StepResult Step1_Gate()
            {
                if (Sim.Stop)   // cooperative shutdown so WaitUntilIdle returns
                {
                    return Done();
                }
                Publish(0.0, "Step1_Gate");
                if (VElapsedSec() < Flow_Runner.GateSec)
                {
                    return Stay();   // re-poll this step next round
                }
                _phaseStart = Flow.Clock.Now();   // reset for Move phase
                Describe("leaving the gate");
                return Next(Step2_Move);
            }

            private StepResult Step2_Move()
            {
                if (Sim.Stop)
                {
                    return Done();
                }
                double frac = VElapsedSec() / Flow.MoveSec;
                if (frac >= 1.0)
                {
                    Publish(100.0, "Step2_Move");
                    _phaseStart = Flow.Clock.Now();
                    Describe("reached the line");
                    return Next(Step3_Rest);
                }
                Publish(frac * 100.0, "Step2_Move");
                return Stay();
            }

            private StepResult Step3_Rest()
            {
                if (Sim.Stop)
                {
                    return Done();
                }
                Publish(100.0, "Step3_Rest");
                if (VElapsedSec() < Flow_Runner.RestSec)
                {
                    return Stay();
                }
                Flow.Lap += 1;                   // one full lap completed
                _phaseStart = Flow.Clock.Now();
                return Next(Step1_Gate);         // loop forever (until Sim.Stop)
            }
        }
    }

    // ========================================================================
    // Flow_View - the dashboard renderer, itself a uniflow module (uf_view.*)
    // ========================================================================
    //
    // FEATURE FOCUS: a renderer is just another flow on the same pump. It reads
    // every runner's row with plain access (no lock) because they share the
    // thread. Its frame cadence uses a REAL-time UFTimer (default ctor, not the
    // virtual clock) - so when the user pauses the sim (clock frozen), the runners
    // stop but the dashboard keeps redrawing and shows [PAUSED].
    internal sealed class Flow_View : Module
    {
        private static readonly string Sep = "  " + new string('-', 60);

        public readonly Task_Draw CtxDraw;

        public Flow_View(Runtime rt)
            : base(rt, "Flow_View")
        {
            CtxDraw = new Task_Draw();
            AddTask(CtxDraw);
        }

        public sealed class Task_Draw : Task<Flow_View>
        {
            private UFTimer _fps = null!;

            // Real-clock throttle (keeps drawing while the sim clock is paused).
            // A default UFTimer reads the process real clock, NOT the virtual one.
            protected override void OnEnter()
            {
                _fps = new UFTimer();
                _fps.Restart();
            }

            protected override StepResult Entry()
            {
                return Step1_Draw();
            }

            private StepResult Step1_Draw()
            {
                if (Sim.Stop)
                {
                    return Done();
                }
                // Throttle to ~30 fps on REAL time, so the dashboard keeps
                // refreshing even while the sim clock is frozen.
                if (_fps.Passed(0.033))
                {
                    _fps.Restart();
                    Render();
                }
                return Stay();
            }

            private void Render()
            {
                var sb = new StringBuilder();

                // Save the user's cursor (sitting on the prompt line), redraw the
                // dashboard above it at fixed positions, then restore it. The
                // prompt row is never touched.
                sb.Append(Ansi.SaveCursor);

                void Put(int row, string text)
                {
                    sb.Append(Ansi.At(row, 1)).Append(Ansi.ClearLine).Append(text);
                }

                Put(1, "  " + Ansi.Bold + "uniflow simulator  " + Ansi.Reset
                       + Ansi.Dim + "v" + Info.Version + Ansi.Reset);
                Put(2, Sep);

                // Header reads live scale/freeze straight off the VirtualClock.
                VirtualClock clk = Flow.Clock;
                string status;
                if (clk.Frozen())
                {
                    status = "  " + Ansi.Yellow + "[PAUSED] " + Ansi.Reset;
                }
                else
                {
                    status = "  " + Ansi.Green + "[RUNNING]" + Ansi.Reset;
                }
                status += "   speed " + Ansi.Cyan + "x"
                          + clk.Scale().ToString("F2") + Ansi.Reset
                          + "      " + Ansi.Gray
                          + "pause | resume | speed <n> | quit" + Ansi.Reset;
                Put(3, status);
                Put(4, Sep);

                for (int i = 0; i < Sim.RunnerCount; i++)
                {
                    RunnerRow r = Sim.Rows[i];
                    string line = "  " + r.Name.PadRight(8)
                                  + " lap " + r.Lap.ToString().PadLeft(2)
                                  + "  [" + Ansi.Green + Ansi.Bar(r.Percent / 100.0, 20)
                                  + Ansi.Reset + "] "
                                  + ((int)(r.Percent + 0.5)).ToString().PadLeft(3) + "%  "
                                  + Ansi.Dim + r.Step + Ansi.Reset;
                    Put(5 + i, line);
                }

                Put(5 + Sim.RunnerCount, Sep);

                sb.Append(Ansi.RestoreCursor);
                Console.Out.Write(sb.ToString());
                Console.Out.Flush();
            }
        }
    }

    // ========================================================================
    // App - the Runtime plus every module (app.h). Two-phase init.
    // ========================================================================
    internal sealed class App
    {
        public readonly Runtime Rt;
        private readonly Flow_View _view;
        private readonly Flow_Runner[] _runners;

        public App()
        {
            // Silent runtime: this app OWNS the console (the dashboard), so the
            // default ConsoleObserver's trace output must be suppressed. A plain
            // 'new Observer()' is the silent observer.
            //
            // All-Stay rounds (everyone polling) should be short so motion and the
            // ~30 fps renderer stay smooth (sleeps are in SECONDS).
            var cfg = new Config
            {
                IdleSleepSec = 0.001,
                StaySleepSec = 0.005,
                StepIntervalSleepSec = 0.0,
            };
            Rt = new Runtime(new Runtime.Options { Observer = new Observer(), Config = cfg });

            // Phase 1: construct. renderer then runners.
            _view = new Flow_View(Rt);
            _runners = new[]
            {
                new Flow_Runner(Rt, "Atlas", 0, 3.8),
                new Flow_Runner(Rt, "Bolt", 1, 2.6),
                new Flow_Runner(Rt, "Comet", 2, 4.6),
                new Flow_Runner(Rt, "Dash", 3, 3.2),
                new Flow_Runner(Rt, "Echo", 4, 5.2),
            };
        }

        public void Start()
        {
            // Phase 2: launch every task. Each StartFlow() puts one task on the pump.
            _view.CtxDraw.StartFlow();
            foreach (Flow_Runner r in _runners)
            {
                r.CtxRun.StartFlow();
            }
        }

        public void Shutdown()
        {
            Sim.Stop = true;   // every step checks this and returns Done()
            Rt.Wake();         // nudge the pump out of any sleep
            foreach (Flow_Runner r in _runners)
            {
                r.WaitUntilIdle();
            }
            _view.WaitUntilIdle();
            Rt.Stop();
        }
    }

    // ========================================================================
    // Program - stdin command loop (mirrors main.cpp / main() in the Python port)
    // ========================================================================
    internal static class Program
    {
        // Draw the input prompt on its reserved row. The renderer never touches
        // this row, so what the user types is not overwritten by frame redraws.
        private static void DrawPrompt()
        {
            Console.Out.Write(Ansi.At(Sim.PromptRow, 1) + Ansi.ClearLine + "  "
                              + Ansi.Bold + "Type a command and press Enter" + Ansi.Reset
                              + Ansi.Dim + " (pause | resume | speed <n> | quit): "
                              + Ansi.Reset);
            Console.Out.Flush();
        }

        private static void RunInputLoop(Runtime rt)
        {
            while (true)
            {
                DrawPrompt();
                string? line = Console.In.ReadLine();
                if (line == null)
                {
                    break;   // EOF (e.g. piped input ended) - treat as quit
                }
                string[] parts = line.Split(
                    (char[]?)null, StringSplitOptions.RemoveEmptyEntries);
                if (parts.Length == 0)
                {
                    continue;
                }
                string cmd = parts[0];
                if (cmd == "quit" || cmd == "exit" || cmd == "q")
                {
                    break;
                }
                if (cmd == "pause")
                {
                    rt.Clock.Freeze();   // stop logical time for ALL runners at once
                }
                else if (cmd == "resume")
                {
                    rt.Clock.Resume();
                }
                else if (cmd == "speed")
                {
                    if (parts.Length >= 2)
                    {
                        double n;
                        if (!double.TryParse(parts[1], out n))
                        {
                            n = 0.0;
                        }
                        if (n > 0.0)
                        {
                            rt.Clock.SetScale(n);   // one call rescales every flow's pace
                        }
                    }
                }
                // Unknown commands are ignored; the dashboard keeps running.
            }
        }

        private static int Main()
        {
            Ansi.EnableAnsi();
            Ansi.HideCursor();
            Ansi.Clear();

            var app = new App();
            app.Start();

            try
            {
                RunInputLoop(app.Rt);
            }
            finally
            {
                app.Shutdown();
                Ansi.ShowCursor();
                Console.Out.Write(Ansi.At(Sim.PromptRow + 1, 1) + Ansi.ClearLine
                                  + "  simulator stopped.\n");
                Console.Out.Flush();
            }
            return 0;
        }
    }
}
