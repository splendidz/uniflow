// LineModel.cs - dimensional constants, line state, and the per-pump-tick device
// models for the pick-and-place line. C# port of the Python port's globals.* /
// motor_io_factory.* helpers (cpp/examples/pick_and_place).
//
// NOTE ON MOTION: the C++ port integrates its MotorAxis objects on a side thread.
// Here the axes integrate per pump tick (dt since last poll) inside MotorAxis -
// the simplest faithful approach on one pump thread. No locks: every module
// advances on the one pump thread round-robin.

#nullable enable

using System;

namespace Uniflow.PickAndPlaceExample
{
    // ----- dimensional constants, timing (mirrors globals.*) -----
    internal static class Geometry
    {
        public const double ZONE_A_MM = 200.0;
        public const double ZONE_B_MM = 700.0;
        public const double ZONE_C_MM = 1200.0;
        public const double X_MAX_MM = 1400.0;
        public const double B_SAFETY_GAP_MM = 250.0;
        public const double STAGE_TRAVEL_MM = 80.0;

        public const double Z_UP_MM = 0.0;
        public const double Z_DOWN_MM = 120.0;          // down is positive

        public const double X_SPEED_MM_PER_S = 300.0;
        public const double Z_SPEED_MM_PER_S = 200.0;

        public const double FINGER_OPEN_MM = 24.0;
        public const double PART_WIDTH_MM = 20.0;
        public const double FINGER_SPEED_MM_PER_S = 200.0;

        public static bool InsideZoneB(double xMm)
        {
            return Math.Abs(xMm - ZONE_B_MM) < B_SAFETY_GAP_MM;
        }
    }

    // The machining phases, made explicit so the orchestrator can launch the
    // Stage's tasks one at a time: RawPartLoaded -> (Prepare) -> Prepared ->
    // (Process) -> Machined -> (Cleanup) -> ProcessedPartReady.
    internal static class StageState
    {
        public const string IDLE = "Idle";
        public const string RAW_PART_LOADED = "RawPartLoaded";
        public const string PREPARED = "Prepared";
        public const string MACHINED = "Machined";
        public const string PROCESSED_PART_READY = "ProcessedPartReady";
    }

    // Line-level environment. Plain pump-thread state (the pickers/stage/
    // orchestrator all run on the one pump), except Stop which the main/stdin
    // thread sets.
    internal static class Env
    {
        private static bool _zoneAPart;
        private static int _delivered;
        private static volatile bool _stop;

        public static bool ZoneAHasPart()
        {
            return _zoneAPart;
        }

        public static void CreateFakeZoneAPart()
        {
            _zoneAPart = true;
        }

        public static void ConsumeZoneAPart()
        {
            _zoneAPart = false;
        }

        public static int DeliveredCount()
        {
            return _delivered;
        }

        public static void IncDelivered()
        {
            _delivered += 1;
        }

        public static bool Stop()
        {
            return _stop;
        }

        public static void RequestStop()
        {
            _stop = true;
        }
    }

    // ----- MotorAxis: 1D linear axis integrated per pump tick -----
    // A step only commands (Move/Home) and polls (InPosition). Update(dt) is
    // driven once per pump round by the owning module's DeviceClock.
    internal sealed class MotorAxis
    {
        public readonly string Name;
        private double _pos;
        private double _target;
        private readonly double _speed;
        private readonly double _home;
        private bool _moving;

        public MotorAxis(string name, double initialMm, double speedMmPerS)
        {
            Name = name;
            _pos = initialMm;
            _target = initialMm;
            _speed = speedMmPerS;
            _home = initialMm;
            _moving = false;
        }

        public void Move(double targetMm)
        {
            _target = targetMm;
            _moving = Math.Abs(_pos - targetMm) > 1e-6;
        }

        public void Home()
        {
            Move(_home);
        }

        public double Position()
        {
            return _pos;
        }

        public bool InPosition()
        {
            return !_moving;
        }

        public void Update(double dtS)
        {
            if (!_moving)
            {
                return;
            }
            double remaining = _target - _pos;
            double step = _speed * dtS;
            if (Math.Abs(remaining) <= step)
            {
                _pos = _target;
                _moving = false;
            }
            else
            {
                _pos += remaining > 0.0 ? step : -step;
            }
        }
    }

    // ----- DigitalLatch: a digital input that latches true a random delay after
    // Arm() - the demo's model for a hardware handshake. The delay is counted
    // down by Update(dt).
    internal sealed class DigitalLatch
    {
        public readonly string Name;
        private readonly double _min;
        private readonly double _max;
        private double _remaining;
        private bool _armed;
        private bool _ready;
        private static readonly Random Rng = new Random();

        public DigitalLatch(string name, double minDelayS, double maxDelayS)
        {
            Name = name;
            _min = minDelayS;
            _max = maxDelayS;
            _remaining = 0.0;
            _armed = false;
            _ready = false;
        }

        public void Arm()
        {
            double u;
            lock (Rng)
            {
                u = Rng.NextDouble();
            }
            _remaining = _min + (_max - _min) * u;
            _armed = true;
            _ready = false;
        }

        public void Reset()
        {
            _armed = false;
            _ready = false;
            _remaining = 0.0;
        }

        public bool IsReady()
        {
            return _ready;
        }

        public void Update(double dtS)
        {
            if (!_armed)
            {
                return;
            }
            _remaining -= dtS;
            if (_remaining <= 0.0)
            {
                _ready = true;
                _armed = false;
            }
        }
    }

    // ----- DeviceClock: per-module helper - tracks dt between pump rounds and
    // ticks a device list. Each module calls Tick() once at the top of every step
    // so its borrowed axes advance with real elapsed time. Mirrors the C++ factory
    // thread's loop, folded onto the pump thread.
    internal sealed class DeviceClock
    {
        private readonly System.Collections.Generic.List<Action<double>> _devices
            = new System.Collections.Generic.List<Action<double>>();
        private double? _last;

        public MotorAxis Add(MotorAxis axis)
        {
            _devices.Add(axis.Update);
            return axis;
        }

        public DigitalLatch Add(DigitalLatch latch)
        {
            _devices.Add(latch.Update);
            return latch;
        }

        private static double Mono()
        {
            return System.Diagnostics.Stopwatch.GetTimestamp()
                   / (double)System.Diagnostics.Stopwatch.Frequency;
        }

        public void Tick()
        {
            double now = Mono();
            if (_last == null)
            {
                _last = now;
                return;
            }
            double dt = now - _last.Value;
            _last = now;
            foreach (var update in _devices)
            {
                update(dt);
            }
        }
    }

    // ----- Snapshot: pump -> render hand-off (mirrors snapshot.*). The render
    // thread reads a copy under a lock; the only cross-thread lock in the demo.
    internal sealed class Snapshot
    {
        public double LoadXMm = Geometry.ZONE_A_MM;
        public double LoadZMm = Geometry.Z_UP_MM;
        public bool LoadCarry;
        public string LoadPhase = "-";

        public double UnloadXMm = Geometry.ZONE_C_MM;
        public double UnloadZMm = Geometry.Z_UP_MM;
        public bool UnloadCarry;
        public string UnloadPhase = "-";

        public double StageTableXMm = Geometry.ZONE_B_MM;
        public double StageTableYMm = 0.0;
        public string StageState = PickAndPlaceExample.StageState.IDLE;
        public string StagePhase = "-";

        public bool ZoneAHasPart;
        public int Delivered;

        public Snapshot Clone()
        {
            return (Snapshot)MemberwiseClone();
        }
    }

    // Process-wide snapshot cell + its render lock.
    internal static class SnapshotStore
    {
        public static readonly Snapshot Snap = new Snapshot();
        public static readonly object Mu = new object();

        public static Snapshot Read()
        {
            lock (Mu)
            {
                return Snap.Clone();
            }
        }
    }
}
