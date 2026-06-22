// Program.cs - console raster renderer + App + main.
//
// C# port of cpp/examples/city_traffic/ (CONSOLE renderer only; the C++ build
// also has a Win32 back-end which has no C# counterpart). Mirrors:
//   uf_visualization_console.cpp -> DrawConsole() ANSI raster renderer
//   app.h / main.cpp             -> App + Main (stdin blocks; Enter / EOF quits)
//
// FEATURE FOCUS:
//   - lock-free shared World on one pump: every traffic-light, every vehicle, and
//     the snapshot writer are tasks on ONE Runtime (one pump thread).
//   - console raster renderer: the grid map, per-approach signal lamps, and the
//     moving fleet are rasterised onto a character canvas with 24-bit colour.

#nullable enable

using System;
using System.Collections.Generic;
using System.Text;
using System.Threading;
using Uniflow;
using Uniflow.Examples;

namespace Uniflow.CityTraffic
{
    // ========================================================================
    // console renderer - ANSI raster back-end (uf_visualization_console.cpp)
    // ========================================================================
    //
    // Rasterises the grid map, per-approach signal lamps, and the moving fleet
    // onto a character canvas with 24-bit colour. Cols-per-grid-unit is ~2x rows
    // so the square map looks square despite terminal cells being ~2:1 tall.
    internal static class Renderer
    {
        private const int SX = 24;            // columns per grid unit
        private const int SY = 10;            // rows per grid unit
        private const int Cols = 2 * SX + 1;  // grid spans [0,2]
        private const int Rows = 2 * SY + 1;
        private const int HeaderRows = 3;     // title lines above the map

        private struct Cell
        {
            public char Ch;
            public string Color;   // ANSI SGR prefix; empty = default
        }

        private static int Col(double gx)
        {
            return (int)Math.Round(gx * SX, MidpointRounding.AwayFromZero);
        }

        private static int Row(double gy)
        {
            return (int)Math.Round(gy * SY, MidpointRounding.AwayFromZero);
        }

        private static string LampColor(LightColor c)
        {
            switch (c)
            {
                case LightColor.Green: return Ansi.Fg(60, 220, 100);
                case LightColor.Yellow: return Ansi.Fg(240, 200, 60);
                default: return Ansi.Fg(170, 70, 70);   // dim red
            }
        }

        private static void Put(Cell[] canvas, int r, int c, char ch, string col)
        {
            if (r < 0 || r >= Rows || c < 0 || c >= Cols)
            {
                return;
            }
            canvas[r * Cols + c].Ch = ch;
            canvas[r * Cols + c].Color = col;
        }

        public static void DrawConsole(Snapshot s)
        {
            var canvas = new Cell[Rows * Cols];
            for (int i = 0; i < canvas.Length; i++)
            {
                canvas[i].Ch = ' ';
                canvas[i].Color = "";
            }
            string road = Ansi.Gray;

            // 1) roads: a dim line of '.' between each pair of connected nodes.
            foreach (var e in Map.Edges)
            {
                Node a = Map.NodeById(e.A);
                Node b = Map.NodeById(e.B);
                int ca = Col(a.Gx), ra = Row(a.Gy);
                int cb = Col(b.Gx), rb = Row(b.Gy);
                if (ra == rb)   // horizontal road
                {
                    for (int c = Math.Min(ca, cb); c <= Math.Max(ca, cb); c++)
                    {
                        Put(canvas, ra, c, '.', road);
                    }
                }
                else            // vertical road
                {
                    for (int r = Math.Min(ra, rb); r <= Math.Max(ra, rb); r++)
                    {
                        Put(canvas, r, ca, '.', road);
                    }
                }
            }

            // 2) signal lamps: one 'O' per approach, placed a few cells out from
            //    the junction toward the approaching road, lit by that approach.
            foreach (Node n in Map.Nodes)
            {
                if (n.Kind == NodeKind.Corner)
                {
                    continue;
                }
                if (n.Id >= s.Signals.Count)
                {
                    continue;
                }
                SignalState st = s.Signals[n.Id];
                int nc = Col(n.Gx), nr = Row(n.Gy);
                foreach (int nb in Map.NeighborsOf(n.Id))
                {
                    Node m = Map.NodeById(nb);
                    int dc = (m.Gx > n.Gx ? 1 : 0) - (m.Gx < n.Gx ? 1 : 0);
                    int dr = (m.Gy > n.Gy ? 1 : 0) - (m.Gy < n.Gy ? 1 : 0);
                    Axis ax = World.AxisOfEdge(n, m);
                    string col = LampColor(World.StraightLight(st, ax));
                    Put(canvas, nr + dr * 2, nc + dc * 3, 'O', col);
                }
            }

            // 3) nodes: junctions show their label initial (X / T), corners '+'.
            string nodeCol = Ansi.Bold + Ansi.Fg(225, 230, 240);
            foreach (Node n in Map.Nodes)
            {
                char ch = n.Kind == NodeKind.Corner
                    ? '+'
                    : (n.Label.Length > 0 ? n.Label[0] : '#');
                Put(canvas, Row(n.Gy), Col(n.Gx), ch, nodeCol);
            }

            // 4) vehicles (drawn last, on top): an arrow in the car's own colour,
            //    pointing the way it is heading.
            foreach (VehicleView v in s.Vehicles)
            {
                char arrow;
                if (Math.Abs(v.Dx) >= Math.Abs(v.Dy))
                {
                    arrow = v.Dx >= 0 ? '>' : '<';
                }
                else
                {
                    arrow = v.Dy >= 0 ? 'v' : '^';   // grid y grows downward
                }
                Put(canvas, Row(v.Gy), Col(v.Gx), arrow, Ansi.Fg(v.R, v.G, v.B));
            }

            // compose one frame and write it in a single flush to avoid flicker.
            var outb = new StringBuilder();
            outb.Append(Ansi.At(1, 1)).Append(Ansi.ClearLine);
            outb.Append("  ").Append(Ansi.Bold).Append("uniflow city traffic  ")
                .Append(Ansi.Reset).Append(Ansi.Dim).Append("v").Append(Info.Version)
                .Append(Ansi.Reset);
            outb.Append(Ansi.At(2, 1)).Append(Ansi.ClearLine).Append("  ").Append(Ansi.Gray)
                .Append("one pump thread: signals, acceleration, car-ahead spacing, "
                        + "intersection yielding").Append(Ansi.Reset);
            outb.Append(Ansi.At(3, 1)).Append(Ansi.ClearLine).Append("  ").Append(Ansi.Dim)
                .Append("press Enter to quit").Append(Ansi.Reset);

            for (int r = 0; r < Rows; r++)
            {
                outb.Append(Ansi.At(r + 1 + HeaderRows, 1)).Append(Ansi.ClearLine);
                string cur = "";   // current active colour to minimise escape spam
                for (int c = 0; c < Cols; c++)
                {
                    Cell cell = canvas[r * Cols + c];
                    if (cell.Color != cur)
                    {
                        outb.Append(cell.Color.Length == 0 ? Ansi.Reset : cell.Color);
                        cur = cell.Color;
                    }
                    outb.Append(cell.Ch);
                }
                outb.Append(Ansi.Reset);
            }

            Console.Out.Write(outb.ToString());
            Console.Out.Flush();
        }

        // Render on a background thread; the main thread blocks on stdin so a
        // single Enter quits. The render thread only READS the snapshot (locked).
        public static void RunVisualisationConsole()
        {
            Ansi.EnableAnsi();
            Ansi.HideCursor();
            Ansi.Clear();

            var done = new ManualResetEventSlim(false);

            var render = new Thread(() =>
            {
                while (!done.IsSet && !Geo.Stop)
                {
                    Snapshot s;
                    lock (World.SnapMu)
                    {
                        s = new Snapshot
                        {
                            Signals = new List<SignalState>(World.Snap.Signals),
                            Vehicles = new List<VehicleView>(World.Snap.Vehicles),
                        };
                    }
                    DrawConsole(s);
                    Thread.Sleep(33);   // ~30 fps
                }
            })
            { Name = "render", IsBackground = true };
            render.Start();

            Console.In.ReadLine();   // any Enter (or EOF) quits
            done.Set();
            render.Join(TimeSpan.FromSeconds(1.0));

            Ansi.ShowCursor();
            Console.Out.Write(Ansi.At(Rows + HeaderRows + 2, 1) + Ansi.ClearLine
                              + "  city_traffic stopped.\n");
            Console.Out.Flush();
        }
    }

    // ========================================================================
    // App - the Runtime plus every module (app.h). Two-phase init.
    // ========================================================================
    internal sealed class App
    {
        public readonly Runtime Rt;
        private readonly Flow_Visualization _viz;
        private readonly List<Flow_TrafficLight> _lights = new List<Flow_TrafficLight>();
        private readonly List<Flow_Vehicle> _vehicles = new List<Flow_Vehicle>();

        public App()
        {
            // Silent runtime: the ANSI renderer OWNS stdout, so the default
            // ConsoleObserver's per-step trace must be suppressed. Short all-Stay
            // naps keep motion and the renderer smooth (sleeps in SECONDS).
            var cfg = new Config
            {
                IdleSleep = 0.001,
                StaySleep = 0.005,
                StepIntervalSleep = 0.0,
            };
            Rt = new Runtime(new Runtime.Options { Observer = new Observer(), Config = cfg });

            // Phase 1: construct. visualisation, then one light per junction, then
            // the fleet.
            _viz = new Flow_Visualization(Rt);

            foreach (Node n in Map.Nodes)
            {
                if (n.Kind != NodeKind.Corner)
                {
                    _lights.Add(new Flow_TrafficLight(Rt, n.Id));
                }
            }

            BuildFleet();
        }

        private void BuildFleet()
        {
            const int n = 15;
            int eCount = Map.Edges.Length;
            World.InitVehicles(n);
            for (int i = 0; i < n; i++)
            {
                var (ea, eb) = Map.Edges[i % eCount];
                int slot = i / eCount;                 // 0 or 1 cars per edge
                double dist0 = 0.22 + 0.45 * slot;
                var (r, g, b) = Hsv((i * 137.5) % 360.0, 0.62, 0.96);  // golden angle
                _vehicles.Add(new Flow_Vehicle(Rt, i, ea, eb, dist0, r, g, b));
            }
        }

        public void Start()
        {
            // Phase 2: launch every task. Each StartFlow() puts one task on the pump.
            _viz.CtxSnapshot.StartFlow();
            foreach (Flow_TrafficLight light in _lights)
            {
                light.CtxSignal.StartFlow();
            }
            foreach (Flow_Vehicle v in _vehicles)
            {
                v.CtxDrive.StartFlow();
            }
        }

        public void Shutdown()
        {
            Geo.Stop = true;    // every step checks this and returns Done()
            Rt.Wake();          // nudge the pump out of any sleep
            foreach (Flow_Vehicle v in _vehicles)
            {
                v.WaitUntilIdle();
            }
            foreach (Flow_TrafficLight light in _lights)
            {
                light.WaitUntilIdle();
            }
            _viz.WaitUntilIdle();
            Rt.Stop();
        }

        // HSV -> (r,g,b) 0..255, for spreading distinct car colours.
        private static (int, int, int) Hsv(double h, double s, double v)
        {
            double c = v * s;
            double x = c * (1.0 - Math.Abs((h / 60.0) % 2.0 - 1.0));
            double m = v - c;
            double rr, gg, bb;
            if (h < 60) { rr = c; gg = x; bb = 0; }
            else if (h < 120) { rr = x; gg = c; bb = 0; }
            else if (h < 180) { rr = 0; gg = c; bb = x; }
            else if (h < 240) { rr = 0; gg = x; bb = c; }
            else if (h < 300) { rr = x; gg = 0; bb = c; }
            else { rr = c; gg = 0; bb = x; }
            return ((int)((rr + m) * 255.0), (int)((gg + m) * 255.0), (int)((bb + m) * 255.0));
        }
    }

    // ========================================================================
    // main (main.cpp): construct modules, arm flows, render, shut down.
    // ========================================================================
    internal static class Program
    {
        private static int Main()
        {
            var app = new App();   // phase 1: Runtime + modules constructed
            app.Start();           // phase 2: flows armed

            try
            {
                Renderer.RunVisualisationConsole();   // main-thread render loop
            }
            finally
            {
                app.Shutdown();
            }
            return 0;
        }
    }
}
