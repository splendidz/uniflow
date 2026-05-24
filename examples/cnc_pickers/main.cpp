// ======================================================================
//  cnc_pickers - demo_concept.md problem 2, solved with uniflow.
//
//  CNC-style line:
//    - Raw parts appear at zone A (Load) at random intervals (1-5 s).
//    - LoadPicker carries a raw part A -> B.
//    - Stage (machining HW at zone B) talks to fake HW, waits for ready,
//      processes for ~5 s, then runs a cleanup handshake and returns its
//      table to the pick position.
//    - UnloadPicker carries the finished part B -> C (Unload).
//
//  Two pickers must never share the B zone - one must clear B (by at least
//  the safety gap) before the other enters.
//
//  Architecture (what this example tries to teach):
//    - LoadPicker and UnloadPicker are SEPARATE classes. They share helpers
//      but their job, source/dest, and gating conditions differ enough that
//      forcing them into one class hides intent.
//    - The Orchestrator is a singleton module that owns line-level
//      scheduling. Pickers and Stage do NOT decide what to do next - they
//      execute the orchestrator's command. They expose Ready/Idle queries
//      and accept commands via plain method calls (same pump thread, no
//      lock needed).
//    - Stage is a sequence of distinct steps (SendStart, WaitReady,
//      Processing, Cleanup, MoveToPickPos). Timing is wall-clock, never
//      frame-count. SendStart and Cleanup fan out to the thread pool via
//      UF_ASYNC to simulate a real HW handshake.
//    - Motion uses a MotorAxis helper exposing InPosition(), which only
//      returns true after the axis has held the target within tolerance
//      for a settling time - matching how real position feedback works.
//
//  Visualisation: a Win32 window on Windows; a console animation elsewhere.
//
//  Build (from the repo root):
//    cl /std:c++17 /EHsc /utf-8 /I include examples\cnc_pickers\main.cpp ^
//       /Fe:build\cnc_pickers.exe /link user32.lib gdi32.lib
//    g++ -std=c++17 -O2 -pthread -I include ^
//       examples/cnc_pickers/main.cpp -o build/cnc_pickers
// ======================================================================
#include "uniflow.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>

using namespace std::chrono_literals;
using uniflow::Clock;
using uniflow::Duration;
using uniflow::FlowStats;
using uniflow::StepAction;
using uniflow::TimePoint;
using uniflow::TraceEntry;
using uniflow::to_ms;

// Visualisation backend: Win32 window on Windows, console animation elsewhere.
// Define UNIFLOW_CONSOLE_VIZ to force the console backend on Windows too.
#if defined(_WIN32) && !defined(UNIFLOW_CONSOLE_VIZ)
#define UNIFLOW_WIN32_VIZ 1
#else
#define UNIFLOW_WIN32_VIZ 0
#endif

#if UNIFLOW_WIN32_VIZ
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

// ======================================================================
//  LineGeometry - everything dimensional about the line.
//
//  Constants carry their unit in the name (_mm / _ms / _mm_per_s). The
//  static-only class is the single namespace for them: no loose globals,
//  no surprises about what owns what.
// ======================================================================
class LineGeometry
{
public:
    // X-axis layout (millimetres, world coordinates).
    static constexpr double kZoneA_mm        = 200.0;  // Load
    static constexpr double kZoneB_mm        = 700.0;  // Stage (machining)
    static constexpr double kZoneC_mm        = 1200.0; // Unload
    static constexpr double kXMax_mm         = 1400.0;
    static constexpr double kBSafetyGap_mm   = 250.0;  // exclusion zone radius
    static constexpr double kStageTravel_mm  = 80.0;   // table travel from pick pos

    // Z-axis (down is positive in this toy model).
    static constexpr double kZUp_mm          = 0.0;
    static constexpr double kZDown_mm        = 120.0;

    // Motion speed (millimetres per second).
    static constexpr double kXSpeed_mm_per_s = 1000.0;
    static constexpr double kZSpeed_mm_per_s = 560.0;

    // Tolerance + settle time for the InPosition predicate.
    static constexpr double   kPosTolerance_mm = 0.5;
    static constexpr Duration kPosSettleTime   = 80ms;

    // True iff `x_mm` is inside the contested B zone.
    static bool InsideZoneB(double x_mm)
    {
        return std::fabs(x_mm - kZoneB_mm) < kBSafetyGap_mm;
    }

    LineGeometry() = delete;
};

// ======================================================================
//  LineTiming - scheduling cadence (how often modules poll / re-tick).
// ======================================================================
class LineTiming
{
public:
    static constexpr Duration kPoll_fast    = 10ms;   // motion / position polls
    static constexpr Duration kPoll_slow    = 50ms;   // ready / state polls
    static constexpr Duration kVizTick      = 16ms;   // visualisation cadence
    static constexpr Duration kSchedTick    = 30ms;   // orchestrator cadence

    // Raw-part inter-arrival range at zone A.
    static constexpr Duration kLoaderMinGap = 1000ms;
    static constexpr Duration kLoaderMaxGap = 5000ms;

    LineTiming() = delete;
};

// ======================================================================
//  LineState - line-level state several modules touch.
// ======================================================================
class LineState
{
public:
    static bool ZoneAHasPart()    { return zone_a_part_; }
    static void SpawnZoneAPart()  { zone_a_part_ = true; }
    static void ConsumeZoneAPart(){ zone_a_part_ = false; }

    static int  DeliveredCount()  { return delivered_; }
    static void IncDelivered()    { ++delivered_; }

    static bool Stop()        { return stop_.load(std::memory_order_relaxed); }
    static void RequestStop() { stop_.store(true, std::memory_order_relaxed); }

    LineState() = delete;

private:
    static inline bool             zone_a_part_ = false;
    static inline int              delivered_   = 0;
    static inline std::atomic<bool> stop_{false};
};

// ======================================================================
//  HwSimulator - fakes the machining HW's "ready" handshake.
//
//  Stage calls ArmReadySignal() right after the start-command ack. A
//  detached worker thread sleeps a random 500-2500 ms and then flips
//  IsReady() to true AND calls uniflow::NotifyAll() so the pump leaves
//  its cv-wait at once - no need to wait for the next idle_sleep tick.
//
//  This is how a real ISR-style HW callback would integrate: flip the
//  flag, notify the runtime, done. The pump observes the new state on
//  the very next round.
// ======================================================================
class HwSimulator
{
public:
    static void ArmReadySignal()
    {
        ready_.store(false, std::memory_order_release);
        std::thread([]
        {
            // thread_local RNG so concurrent arms (e.g. across cycles)
            // do not share state.
            static thread_local std::mt19937 rng{std::random_device{}()};
            std::uniform_int_distribution<int> d(500, 2500);
            int ms = d(rng);
            std::this_thread::sleep_for(std::chrono::milliseconds(ms));
            ready_.store(true, std::memory_order_release);
            uniflow::NotifyAll();
        }).detach();
    }
    static bool IsReady()
    {
        return ready_.load(std::memory_order_acquire);
    }

    HwSimulator() = delete;

private:
    static inline std::atomic<bool> ready_{false};
};

// ======================================================================
//  StageState - what the Stage thinks it is doing right now.
// ======================================================================
enum class StageState : uint8_t
{
    Idle,                // no raw part, no processed part
    RawPartLoaded,       // raw part placed; processing not yet started
    Processing,          // HW handshake running, table moving, etc.
    ProcessedPartReady,  // machined part awaiting UnloadPicker
};

inline const char* ToString(StageState s)
{
    switch (s)
    {
    case StageState::Idle:               return "Idle";
    case StageState::RawPartLoaded:      return "RawPartLoaded";
    case StageState::Processing:         return "Processing";
    case StageState::ProcessedPartReady: return "ProcessedPartReady";
    }
    return "?";
}

// ======================================================================
//  MotorAxis - 1D linear axis with feedback.
//
//  The step layer calls Update(speed_mm_per_s) once per pump tick; the
//  helper advances toward the latest target using the wall-clock delta
//  since the previous Update. InPosition() returns true only after the
//  axis has held the target within tolerance for a settling window -
//  this is how a real position-feedback loop reports "in position".
// ======================================================================
class MotorAxis
{
public:
    explicit MotorAxis(double initial_mm)
        : pos_mm_(initial_mm), target_mm_(initial_mm),
          last_update_at_(Clock::now())
    {}

    void SetTarget(double target_mm)
    {
        if (target_mm == target_mm_)
            return;
        target_mm_ = target_mm;
        in_window_since_.reset();
    }

    // Advance the axis by one pump tick toward the current target.
    void Update(double speed_mm_per_s)
    {
        auto   now  = Clock::now();
        double dt_s = std::chrono::duration<double>(now - last_update_at_).count();
        last_update_at_ = now;

        double remaining = target_mm_ - pos_mm_;
        double step      = speed_mm_per_s * dt_s;
        if (std::fabs(remaining) <= step)
            pos_mm_ = target_mm_;
        else
            pos_mm_ += (remaining > 0 ? step : -step);
    }

    // True only after the axis has been within `tol` of the target for at
    // least `settle`. Reset whenever SetTarget moves the goalpost.
    bool InPosition(double tol_mm = LineGeometry::kPosTolerance_mm,
                    Duration settle = LineGeometry::kPosSettleTime) const
    {
        if (std::fabs(pos_mm_ - target_mm_) > tol_mm)
        {
            in_window_since_.reset();
            return false;
        }
        auto now = Clock::now();
        if (!in_window_since_)
            in_window_since_ = now;
        return (now - *in_window_since_) >= settle;
    }

    double Position() const { return pos_mm_; }
    double Target()   const { return target_mm_; }

private:
    double                           pos_mm_;
    double                           target_mm_;
    TimePoint                        last_update_at_;
    mutable std::optional<TimePoint> in_window_since_;
};

// ======================================================================
//  Snapshot - the one cross-thread hand-off (pump -> UI thread).
// ======================================================================
struct Snapshot
{
    double      load_x_mm  = LineGeometry::kZoneA_mm;
    double      load_z_mm  = LineGeometry::kZUp_mm;
    bool        load_carry = false;
    std::string load_phase = "-";

    double      unload_x_mm  = LineGeometry::kZoneC_mm;
    double      unload_z_mm  = LineGeometry::kZUp_mm;
    bool        unload_carry = false;
    std::string unload_phase = "-";

    double      stage_table_x_mm = LineGeometry::kZoneB_mm;
    double      stage_table_y_mm = 0.0;
    StageState  stage_state      = StageState::Idle;
    std::string stage_phase      = "-";

    bool zoneA_has_part = false;
    int  delivered      = 0;
};
static Snapshot   g_snap;
static std::mutex g_snap_mu;

// ======================================================================
//  Forward declarations - the two pickers reference each other.
// ======================================================================
class LoadPicker;
class UnloadPicker;

// ======================================================================
//  Stage - the machining cell at zone B. Single instance.
//
//  Lifecycle is a sequence of flows started by the Orchestrator. One flow
//  == one part processed. The flow runs:
//
//    SendStartHwCommand (async, ~1 s)
//    WaitHwReady        (poll, ~1 s)
//    Processing         (~5 s, time-based)
//    SendCleanupCommand (async, ~0.5s)
//    MoveToPickPos                 - retract table, InPosition wait
//    -> Done; state becomes ProcessedPartReady
//
//  Pickers call ReadyToReceiveRawPart / ReadyToHandOffProcessedPart and
//  never touch the Stage's internal phase.
// ======================================================================
class Stage : public uniflow::Uniflow<Stage>
{
    UF_SINGLETON(Stage);
    //
    // === SINGLE INSTANCE: exactly one Stage cell at zone B. ===
    //
    // UF_SINGLETON above is not boilerplate - it encodes a hardware
    // assumption. A second instance would be a logic error and the macro
    // prevents it at compile time.
    //

public:
    StageState  state() const { return state_; }
    const char* phase() const { return phase_; }
    double      TableX_mm() const { return table_x_axis_.Position(); }
    double      TableY_mm() const { return table_y_offset_mm_; }

    bool ReadyToReceiveRawPart() const
    {
        return state_ == StageState::Idle;
    }
    bool ReadyToHandOffProcessedPart() const
    {
        return state_ == StageState::ProcessedPartReady;
    }

    void OnRawPartReceived()
    {
        state_ = StageState::RawPartLoaded;
        phase_ = "raw part loaded";
    }
    void OnProcessedPartTaken()
    {
        state_ = StageState::Idle;
        phase_ = "empty";
    }

    // Entry: the Orchestrator starts this flow when state is RawPartLoaded.
    StepResult OnProcess_Begin()
    {
        if (state_ != StageState::RawPartLoaded)
            return Fail();
        state_ = StageState::Processing;
        phase_ = "send start cmd";
        return UF_NEXT(OnProcess_SendStartCmd);
    }

    StepResult OnProcess_SendStartCmd()
    {
        if (LineState::Stop())
            return Done();
        UF_ASYNC(SimulateStartCmd);
        return UF_NEXT(OnProcess_WaitStartCmdAck);
    }
    StepResult OnProcess_WaitStartCmdAck()
    {
        auto r = AsyncResult<bool>();
        if (r.failed() || r.is_timeout() || !r.value())
        {
            phase_ = "start cmd failed";
            return Fail();
        }
        phase_ = "wait hw ready";
        // Kick off the fake HW ready handshake: a worker thread will set
        // HwSimulator::IsReady() to true at a random time within 0.5-2.5 s
        // and call uniflow::NotifyAll() so we wake immediately on it.
        HwSimulator::ArmReadySignal();
        return UF_NEXT(OnProcess_WaitHwReady, uniflow::UFTimer{});
    }
    StepResult OnProcess_WaitHwReady(uniflow::UFTimer& t)
    {
        if (LineState::Stop())              return Done();
        if (t.TimedOut(3000ms))             { phase_ = "hw ready timeout";
                                              return Fail(); }
        if (!HwSimulator::IsReady())        return Wait();   // poll cadence: default

        phase_             = "processing";
        table_y_offset_mm_ = 0.0;
        table_x_axis_.SetTarget(LineGeometry::kZoneB_mm
                                + LineGeometry::kStageTravel_mm);
        return UF_NEXT(OnProcess_Run, uniflow::UFTimer{});
    }

    StepResult OnProcess_Run(uniflow::UFTimer& t)
    {
        if (LineState::Stop())
            return Done();
        // Stay() / Wait() re-enter THIS step fn: by-ref signature means we
        // touch the SAME UFTimer instance stored in the next-step capture
        // tuple every time, so Elapsed() keeps growing from the moment the
        // step was first armed (and any t.Restart() would persist too).
        auto elapsed = t.Elapsed();
        double frac =
            std::chrono::duration<double>(elapsed).count()
            / std::chrono::duration<double>(kProcessDuration).count();
        if (frac > 1.0) frac = 1.0;
        double sweep_mm =
            LineGeometry::kStageTravel_mm * std::sin(frac * 6.283 * 2.0);
        table_x_axis_.SetTarget(LineGeometry::kZoneB_mm + sweep_mm);
        table_y_offset_mm_ = 40.0 * std::sin(frac * 6.283 * 3.0);
        table_x_axis_.Update(LineGeometry::kXSpeed_mm_per_s);

        if (elapsed >= kProcessDuration)
        {
            phase_ = "send cleanup cmd";
            return UF_NEXT(OnProcess_SendCleanupCmd);
        }
        return Wait(LineTiming::kPoll_fast);
    }

    StepResult OnProcess_SendCleanupCmd()
    {
        if (LineState::Stop())
            return Done();
        UF_ASYNC(SimulateCleanupCmd);
        return UF_NEXT(OnProcess_WaitCleanupAck);
    }
    StepResult OnProcess_WaitCleanupAck()
    {
        auto r = AsyncResult<bool>();
        if (r.failed() || r.is_timeout() || !r.value())
        {
            phase_ = "cleanup failed";
            return Fail();
        }
        phase_ = "return to pick pos";
        table_x_axis_.SetTarget(LineGeometry::kZoneB_mm);
        return UF_NEXT(OnProcess_ReturnToPickPos);
    }
    StepResult OnProcess_ReturnToPickPos()
    {
        if (LineState::Stop())
            return Done();
        table_x_axis_.Update(LineGeometry::kXSpeed_mm_per_s);
        table_y_offset_mm_ = 0.0;
        if (!table_x_axis_.InPosition())
            return Wait(LineTiming::kPoll_fast);

        state_ = StageState::ProcessedPartReady;
        phase_ = "ready to hand off";
        return Done();
    }

    // ---- Static workers handed to UF_ASYNC ---------------------------
    static bool SimulateStartCmd()
    {
        std::this_thread::sleep_for(1000ms);
        return true;
    }
    static bool SimulateCleanupCmd()
    {
        std::this_thread::sleep_for(500ms);
        return true;
    }

private:
    static constexpr Duration kProcessDuration = 5000ms;

    StageState  state_ = StageState::Idle;
    const char* phase_ = "empty";

    // Timing state moved into UFTimer instances threaded through the step
    // chain (see OnProcess_WaitHwReady / OnProcess_Run signatures).
    MotorAxis   table_x_axis_{LineGeometry::kZoneB_mm};
    double      table_y_offset_mm_ = 0.0;
};

// ======================================================================
//  PickerMotion - shared motion + phase state (not a Uniflow base).
//
//  Composed by inheritance into LoadPicker and UnloadPicker. Kept off the
//  Uniflow CRTP base because that base is templated on Derived.
// ======================================================================
class PickerMotion
{
public:
    explicit PickerMotion(double home_x_mm)
        : x_axis_(home_x_mm), z_axis_(LineGeometry::kZUp_mm)
    {}

    double      X_mm()     const { return x_axis_.Position(); }
    double      Z_mm()     const { return z_axis_.Position(); }
    bool        Carrying() const { return carrying_; }
    const char* Phase()    const { return phase_; }

    bool InsideZoneB() const { return LineGeometry::InsideZoneB(X_mm()); }

protected:
    void SetPhase(const char* p) { phase_ = p; }
    void SetCarrying(bool v)     { carrying_ = v; }

    MotorAxis   x_axis_;
    MotorAxis   z_axis_;
    bool        carrying_ = false;
    const char* phase_    = "idle";
};

// ======================================================================
//  LoadPicker - carries a raw part A -> B.
//
//  One flow == one A->B->A round trip. The Orchestrator launches this
//  picker when zone A has a part and the picker is Idle. The picker
//  itself decides to hold at the B-safety-gap boundary if Stage is not
//  yet ready to receive (this gives the "prefetch + wait" behaviour).
// ======================================================================
class LoadPicker : public uniflow::Uniflow<LoadPicker>,
                   public PickerMotion
{
    UF_USES_UNIFLOW(LoadPicker);

public:
    LoadPicker()
        : uniflow::Uniflow<LoadPicker>("LoadPicker"),
          PickerMotion(LineGeometry::kZoneA_mm)
    {}

    StepResult OnLoad_Begin()
    {
        SetPhase("load: start");
        return UF_NEXT(OnLoad_GoToSource);
    }
    StepResult OnLoad_GoToSource()
    {
        if (LineState::Stop()) return Done();
        SetPhase("load: -> A");
        x_axis_.SetTarget(LineGeometry::kZoneA_mm);
        x_axis_.Update(LineGeometry::kXSpeed_mm_per_s);
        if (!x_axis_.InPosition())
            return Wait(LineTiming::kPoll_fast);
        return UF_NEXT(OnLoad_DescendToPick);
    }
    StepResult OnLoad_DescendToPick()
    {
        if (LineState::Stop()) return Done();
        SetPhase("load: pick v");
        z_axis_.SetTarget(LineGeometry::kZDown_mm);
        z_axis_.Update(LineGeometry::kZSpeed_mm_per_s);
        if (!z_axis_.InPosition())
            return Wait(LineTiming::kPoll_fast);

        LineState::ConsumeZoneAPart();
        SetCarrying(true);
        return UF_NEXT(OnLoad_RisingWithPart);
    }
    StepResult OnLoad_RisingWithPart()
    {
        if (LineState::Stop()) return Done();
        SetPhase("load: pick ^");
        z_axis_.SetTarget(LineGeometry::kZUp_mm);
        z_axis_.Update(LineGeometry::kZSpeed_mm_per_s);
        if (!z_axis_.InPosition())
            return Wait(LineTiming::kPoll_fast);
        return UF_NEXT(OnLoad_GoToDest);
    }
    StepResult OnLoad_GoToDest()
    {
        if (LineState::Stop()) return Done();
        // Gate entry into the B exclusion zone: Stage must be ready AND
        // the partner picker must not be inside the gap.
        bool may_enter_B =
            Stage::inst().ReadyToReceiveRawPart() && !PartnerInZoneB();

        if (!InsideZoneB() && !may_enter_B)
        {
            SetPhase("load: wait B clear");
            // Hold at the A-side gap boundary.
            x_axis_.SetTarget(LineGeometry::kZoneB_mm
                              - LineGeometry::kBSafetyGap_mm);
            x_axis_.Update(LineGeometry::kXSpeed_mm_per_s);
            return Wait(LineTiming::kPoll_slow);
        }
        SetPhase("load: -> B");
        x_axis_.SetTarget(LineGeometry::kZoneB_mm);
        x_axis_.Update(LineGeometry::kXSpeed_mm_per_s);
        if (!x_axis_.InPosition())
            return Wait(LineTiming::kPoll_fast);
        return UF_NEXT(OnLoad_DescendToPlace);
    }
    StepResult OnLoad_DescendToPlace()
    {
        if (LineState::Stop()) return Done();
        SetPhase("load: place v");
        z_axis_.SetTarget(LineGeometry::kZDown_mm);
        z_axis_.Update(LineGeometry::kZSpeed_mm_per_s);
        if (!z_axis_.InPosition())
            return Wait(LineTiming::kPoll_fast);

        Stage::inst().OnRawPartReceived();
        SetCarrying(false);
        return UF_NEXT(OnLoad_RisingEmpty);
    }
    StepResult OnLoad_RisingEmpty()
    {
        if (LineState::Stop()) return Done();
        SetPhase("load: place ^");
        z_axis_.SetTarget(LineGeometry::kZUp_mm);
        z_axis_.Update(LineGeometry::kZSpeed_mm_per_s);
        if (!z_axis_.InPosition())
            return Wait(LineTiming::kPoll_fast);
        return UF_NEXT(OnLoad_Retreat);
    }
    StepResult OnLoad_Retreat()
    {
        if (LineState::Stop()) return Done();
        SetPhase("load: retreat");
        x_axis_.SetTarget(LineGeometry::kZoneA_mm);
        x_axis_.Update(LineGeometry::kXSpeed_mm_per_s);
        if (!x_axis_.InPosition())
            return Wait(LineTiming::kPoll_fast);
        SetPhase("load: idle");
        return Done();
    }

private:
    // Defined out-of-line: UnloadPicker must be complete first.
    bool PartnerInZoneB() const;
};

// ======================================================================
//  UnloadPicker - carries the finished part B -> C.
//
//  Same shape as LoadPicker but the SOURCE is the contested B zone, so
//  the entry-to-B check happens on the way in (not just on the dest dip).
// ======================================================================
class UnloadPicker : public uniflow::Uniflow<UnloadPicker>,
                     public PickerMotion
{
    UF_USES_UNIFLOW(UnloadPicker);

public:
    UnloadPicker()
        : uniflow::Uniflow<UnloadPicker>("UnloadPicker"),
          PickerMotion(LineGeometry::kZoneC_mm)
    {}

    StepResult OnUnload_Begin()
    {
        SetPhase("unload: start");
        return UF_NEXT(OnUnload_GoToStage);
    }
    StepResult OnUnload_GoToStage()
    {
        if (LineState::Stop()) return Done();
        bool may_enter_B =
            Stage::inst().ReadyToHandOffProcessedPart() && !PartnerInZoneB();

        if (!InsideZoneB() && !may_enter_B)
        {
            SetPhase("unload: wait B clear");
            x_axis_.SetTarget(LineGeometry::kZoneB_mm
                              + LineGeometry::kBSafetyGap_mm);
            x_axis_.Update(LineGeometry::kXSpeed_mm_per_s);
            return Wait(LineTiming::kPoll_slow);
        }
        SetPhase("unload: -> B");
        x_axis_.SetTarget(LineGeometry::kZoneB_mm);
        x_axis_.Update(LineGeometry::kXSpeed_mm_per_s);
        if (!x_axis_.InPosition())
            return Wait(LineTiming::kPoll_fast);
        return UF_NEXT(OnUnload_DescendToPick);
    }
    StepResult OnUnload_DescendToPick()
    {
        if (LineState::Stop()) return Done();
        SetPhase("unload: pick v");
        z_axis_.SetTarget(LineGeometry::kZDown_mm);
        z_axis_.Update(LineGeometry::kZSpeed_mm_per_s);
        if (!z_axis_.InPosition())
            return Wait(LineTiming::kPoll_fast);

        Stage::inst().OnProcessedPartTaken();
        SetCarrying(true);
        return UF_NEXT(OnUnload_RisingWithPart);
    }
    StepResult OnUnload_RisingWithPart()
    {
        if (LineState::Stop()) return Done();
        SetPhase("unload: pick ^");
        z_axis_.SetTarget(LineGeometry::kZUp_mm);
        z_axis_.Update(LineGeometry::kZSpeed_mm_per_s);
        if (!z_axis_.InPosition())
            return Wait(LineTiming::kPoll_fast);
        return UF_NEXT(OnUnload_GoToUnload);
    }
    StepResult OnUnload_GoToUnload()
    {
        if (LineState::Stop()) return Done();
        SetPhase("unload: -> C");
        x_axis_.SetTarget(LineGeometry::kZoneC_mm);
        x_axis_.Update(LineGeometry::kXSpeed_mm_per_s);
        if (!x_axis_.InPosition())
            return Wait(LineTiming::kPoll_fast);
        return UF_NEXT(OnUnload_DescendToPlace);
    }
    StepResult OnUnload_DescendToPlace()
    {
        if (LineState::Stop()) return Done();
        SetPhase("unload: place v");
        z_axis_.SetTarget(LineGeometry::kZDown_mm);
        z_axis_.Update(LineGeometry::kZSpeed_mm_per_s);
        if (!z_axis_.InPosition())
            return Wait(LineTiming::kPoll_fast);

        LineState::IncDelivered();
        SetCarrying(false);
        return UF_NEXT(OnUnload_RisingEmpty);
    }
    StepResult OnUnload_RisingEmpty()
    {
        if (LineState::Stop()) return Done();
        SetPhase("unload: place ^");
        z_axis_.SetTarget(LineGeometry::kZUp_mm);
        z_axis_.Update(LineGeometry::kZSpeed_mm_per_s);
        if (!z_axis_.InPosition())
            return Wait(LineTiming::kPoll_fast);
        return UF_NEXT(OnUnload_Retreat);
    }
    StepResult OnUnload_Retreat()
    {
        if (LineState::Stop()) return Done();
        SetPhase("unload: retreat");
        x_axis_.SetTarget(LineGeometry::kZoneC_mm);
        x_axis_.Update(LineGeometry::kXSpeed_mm_per_s);
        if (!x_axis_.InPosition())
            return Wait(LineTiming::kPoll_fast);
        SetPhase("unload: idle");
        return Done();
    }

private:
    bool PartnerInZoneB() const;
};

// Cross-picker queries: both classes must be complete first.
inline bool LoadPicker::PartnerInZoneB() const
{
    return UnloadPicker::GetInst().InsideZoneB();
}
inline bool UnloadPicker::PartnerInZoneB() const
{
    return LoadPicker::GetInst().InsideZoneB();
}

// ======================================================================
//  Orchestrator - the line-level scheduler.
//
//  Owns ALL scheduling decisions:
//    - Spawning raw parts at zone A (the old Loader module, absorbed -
//      "a part appears" is a single decision, no flow needed).
//    - Kicking off LoadPicker / UnloadPicker / Stage flows when their
//      precondition is satisfied.
//
//  Pickers and Stage never decide what to do next.
// ======================================================================
class Orchestrator : public uniflow::Uniflow<Orchestrator>
{
    UF_SINGLETON(Orchestrator);

public:
    StepResult OnSchedule_Begin()
    {
        ArmNextLoaderSpawn();
        return UF_NEXT(OnSchedule_Tick);
    }

    StepResult OnSchedule_Tick()
    {
        if (LineState::Stop())
            return Done();

        MaybeSpawnRawPart();
        MaybeStartLoadPicker();
        MaybeStartStageProcessing();
        MaybeStartUnloadPicker();

        return Wait(LineTiming::kSchedTick);
    }

private:
    void ArmNextLoaderSpawn()
    {
        auto min_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          LineTiming::kLoaderMinGap).count();
        auto max_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                          LineTiming::kLoaderMaxGap).count();
        std::uniform_int_distribution<long long> d(min_ms, max_ms);
        next_spawn_at_ = Clock::now() + std::chrono::milliseconds(d(rng_));
    }
    void MaybeSpawnRawPart()
    {
        if (LineState::ZoneAHasPart())
            return;
        if (Clock::now() < next_spawn_at_)
            return;
        LineState::SpawnZoneAPart();
        ArmNextLoaderSpawn();
    }

    void MaybeStartLoadPicker()
    {
        auto& picker = LoadPicker::GetInst();
        if (!picker.IsIdle())
            return;
        if (!LineState::ZoneAHasPart())
            return;
        // Launch even if Stage is not yet Idle - the picker will park at
        // the B-zone boundary. This is the "prefetch" behaviour the old
        // version was missing.
        picker.StartFlow(&LoadPicker::OnLoad_Begin);
    }
    void MaybeStartUnloadPicker()
    {
        auto& picker = UnloadPicker::GetInst();
        if (!picker.IsIdle())
            return;
        auto st = Stage::inst().state();
        if (st != StageState::Processing && st != StageState::ProcessedPartReady)
            return;
        picker.StartFlow(&UnloadPicker::OnUnload_Begin);
    }
    void MaybeStartStageProcessing()
    {
        auto& stage = Stage::inst();
        if (!stage.IsIdle())
            return;
        if (stage.state() != StageState::RawPartLoaded)
            return;
        stage.StartFlow(&Stage::OnProcess_Begin);
    }

    std::mt19937 rng_{std::random_device{}()};
    TimePoint    next_spawn_at_;
};

// ======================================================================
//  Viz - snapshots the whole line into g_snap every frame.
// ======================================================================
class Viz : public uniflow::Uniflow<Viz>
{
    UF_SINGLETON(Viz);

public:
    StepResult OnViz_Begin() { return UF_NEXT(OnViz_Tick); }

    StepResult OnViz_Tick()
    {
        if (LineState::Stop())
            return Done();
        const auto& load   = LoadPicker::GetInst();
        const auto& unload = UnloadPicker::GetInst();
        const auto& stage  = Stage::inst();
        {
            std::lock_guard<std::mutex> lk(g_snap_mu);
            g_snap.load_x_mm        = load.X_mm();
            g_snap.load_z_mm        = load.Z_mm();
            g_snap.load_carry       = load.Carrying();
            g_snap.load_phase       = load.Phase();
            g_snap.unload_x_mm      = unload.X_mm();
            g_snap.unload_z_mm      = unload.Z_mm();
            g_snap.unload_carry     = unload.Carrying();
            g_snap.unload_phase     = unload.Phase();
            g_snap.stage_table_x_mm = stage.TableX_mm();
            g_snap.stage_table_y_mm = stage.TableY_mm();
            g_snap.stage_state      = stage.state();
            g_snap.stage_phase      = stage.phase();
            g_snap.zoneA_has_part   = LineState::ZoneAHasPart();
            g_snap.delivered        = LineState::DeliveredCount();
        }
        return Wait(LineTiming::kVizTick);
    }
};

// Read a consistent copy of the line state from the UI thread.
static Snapshot ReadSnapshot()
{
    std::lock_guard<std::mutex> lk(g_snap_mu);
    return g_snap;
}

// ======================================================================
//  LineLogObserver - mirrors flow events to console AND to a log file.
//
//  The file is opened and closed on every emission. Slow, but the line
//  can never be lost on a crash. Time cost was explicitly OK.
// ======================================================================
class LineLogObserver : public uniflow::IUniflowObserver
{
public:
    explicit LineLogObserver(std::string file_path)
        : path_(std::move(file_path))
    {}

    void OnFlowStarted(std::string_view obj) override
    {
        Emit(obj, "FLOW START", "");
    }
    void OnStepRan(std::string_view obj, std::string_view step,
                   int ordinal, Duration cpu) override
    {
        std::ostringstream os;
        os << "#" << ordinal << " " << step << " cpu=" << FmtMs(cpu);
        Emit(obj, "STEP", os.str());
    }
    void OnAsyncSubmitted(std::string_view obj, std::string_view step,
                          std::string_view job) override
    {
        std::ostringstream os;
        os << job << " (from " << step << ")";
        Emit(obj, "ASYNC SUBMIT", os.str());
    }
    void OnAsyncCompleted(std::string_view obj, std::string_view job,
                          Duration wait, bool had_error, bool timed_out) override
    {
        std::ostringstream os;
        os << job << " wait=" << FmtMs(wait);
        if (timed_out)      os << " [TIMEOUT]";
        else if (had_error) os << " [ERROR]";
        else                os << " [ok]";
        Emit(obj, "ASYNC DONE", os.str());
    }
    void OnSlowCpuStep(std::string_view obj, std::string_view step,
                       Duration cpu) override
    {
        std::ostringstream os;
        os << "SLOW CPU step " << step << " took " << FmtMs(cpu);
        Emit(obj, "WARN", os.str());
    }
    void OnSlowAsync(std::string_view obj, std::string_view job,
                     Duration wait_so_far) override
    {
        std::ostringstream os;
        os << "SLOW ASYNC " << job << " still pending after "
           << FmtMs(wait_so_far);
        Emit(obj, "WARN", os.str());
    }
    void OnFlowEnded(std::string_view obj, StepAction terminal_action,
                     int reached_ordinal, const std::vector<TraceEntry>&,
                     Duration wall_clock, Duration total_cpu,
                     Duration total_async_wait, const FlowStats&) override
    {
        std::ostringstream os;
        os << (terminal_action == StepAction::Done ? "DONE" : "FAILED")
           << " reached=#" << reached_ordinal
           << " wall=" << FmtMs(wall_clock)
           << " cpu="  << FmtMs(total_cpu)
           << " async="<< FmtMs(total_async_wait);
        Emit(obj, "FLOW END", os.str());
    }

private:
    static std::string FmtMs(Duration d)
    {
        std::ostringstream os;
        os.setf(std::ios::fixed);
        os.precision(2);
        os << to_ms(d) << "ms";
        return os.str();
    }

    void Emit(std::string_view obj, const char* tag, const std::string& msg)
    {
        std::lock_guard<std::mutex> lk(mu_);
        std::ostringstream os;
        os << "[" << obj << "] " << tag;
        if (!msg.empty())
            os << " " << msg;
        std::string line = os.str();

        std::cout << line << "\n";
        std::cout.flush();

        // Open / append / close on every line. Survives a crash.
        std::ofstream fout(path_, std::ios::app);
        if (fout)
            fout << line << "\n";
    }

    std::mutex  mu_;
    std::string path_;
};

// ======================================================================
//  Line lifecycle
// ======================================================================
static void StartLine()
{
    Stage::inst();           // lazy create
    Viz::inst();
    Orchestrator::inst();

    Viz::inst().StartFlow(&Viz::OnViz_Begin);
    Orchestrator::inst().StartFlow(&Orchestrator::OnSchedule_Begin);
}

static void StopLine()
{
    LineState::RequestStop();
    Orchestrator::inst().WaitUntilIdle();
    LoadPicker::GetInst().WaitUntilIdle();
    UnloadPicker::GetInst().WaitUntilIdle();
    Stage::inst().WaitUntilIdle();
    Viz::inst().WaitUntilIdle();
}

// ======================================================================
//  Visualisation
// ======================================================================
#if UNIFLOW_WIN32_VIZ

static int SX(double x) { return 60 + static_cast<int>(x / LineGeometry::kXMax_mm * 860.0); }
static int SZ(double z) { return 96 + static_cast<int>(z / LineGeometry::kZDown_mm * 210.0); }

static void DrawScene(HDC hdc, const RECT& rc)
{
    HDC     mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HBITMAP old = static_cast<HBITMAP>(SelectObject(mem, bmp));

    HBRUSH bg = CreateSolidBrush(RGB(24, 26, 32));
    FillRect(mem, &rc, bg);
    DeleteObject(bg);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(210, 214, 222));

    Snapshot s = ReadSnapshot();

    HPEN rail = CreatePen(PS_SOLID, 3, RGB(70, 74, 84));
    HPEN oldp = static_cast<HPEN>(SelectObject(mem, rail));
    MoveToEx(mem, 50, 70, nullptr); LineTo(mem, 930, 70);
    MoveToEx(mem, 50, 86, nullptr); LineTo(mem, 930, 86);
    SelectObject(mem, oldp);
    DeleteObject(rail);

    auto zone = [&](double zx, const char* label, COLORREF c)
    {
        RECT   z{SX(zx) - 56, 330, SX(zx) + 56, 372};
        HBRUSH b = CreateSolidBrush(c);
        FillRect(mem, &z, b);
        DeleteObject(b);
        TextOutA(mem, SX(zx) - 50, 376, label, lstrlenA(label));
    };
    zone(LineGeometry::kZoneA_mm, "A  Load",   RGB(40, 60, 90));
    zone(LineGeometry::kZoneB_mm, "B  Stage",  RGB(70, 55, 90));
    zone(LineGeometry::kZoneC_mm, "C  Unload", RGB(40, 80, 60));

    HPEN span = CreatePen(PS_DOT, 1, RGB(120, 90, 130));
    oldp      = static_cast<HPEN>(SelectObject(mem, span));
    MoveToEx(mem, SX(LineGeometry::kZoneB_mm - LineGeometry::kBSafetyGap_mm), 60, nullptr);
    LineTo  (mem, SX(LineGeometry::kZoneB_mm - LineGeometry::kBSafetyGap_mm), 330);
    MoveToEx(mem, SX(LineGeometry::kZoneB_mm + LineGeometry::kBSafetyGap_mm), 60, nullptr);
    LineTo  (mem, SX(LineGeometry::kZoneB_mm + LineGeometry::kBSafetyGap_mm), 330);
    SelectObject(mem, oldp);
    DeleteObject(span);

    {
        int    sx = SX(s.stage_table_x_mm);
        int    sy = 318 + static_cast<int>(s.stage_table_y_mm / 40.0 * 8.0);
        RECT   t{sx - 34, sy - 12, sx + 34, sy + 12};
        COLORREF c =
            s.stage_state == StageState::Processing         ? RGB(200, 120, 60) :
            s.stage_state == StageState::ProcessedPartReady ? RGB(90, 170, 110) :
            s.stage_state == StageState::RawPartLoaded      ? RGB(180, 160, 60) :
                                                              RGB(90, 94, 104);
        HBRUSH b = CreateSolidBrush(c);
        FillRect(mem, &t, b);
        DeleteObject(b);
    }

    if (s.zoneA_has_part)
    {
        RECT   p{SX(LineGeometry::kZoneA_mm) - 11, 308, SX(LineGeometry::kZoneA_mm) + 11, 330};
        HBRUSH b = CreateSolidBrush(RGB(210, 180, 70));
        FillRect(mem, &p, b);
        DeleteObject(b);
    }

    auto picker = [&](double px, double pz, bool carry, int rail_y,
                      COLORREF c, const char* tag, const std::string& phase)
    {
        int  hx = SX(px), hy = SZ(pz);
        HPEN arm = CreatePen(PS_SOLID, 5, c);
        oldp     = static_cast<HPEN>(SelectObject(mem, arm));
        MoveToEx(mem, hx, rail_y, nullptr);
        LineTo  (mem, hx, hy);
        SelectObject(mem, oldp);
        DeleteObject(arm);

        HBRUSH hb = CreateSolidBrush(c);
        RECT   head{hx - 16, hy - 9, hx + 16, hy + 9};
        FillRect(mem, &head, hb);
        DeleteObject(hb);

        if (carry)
        {
            RECT   part{hx - 10, hy + 9, hx + 10, hy + 29};
            HBRUSH pb = CreateSolidBrush(RGB(210, 180, 70));
            FillRect(mem, &part, pb);
            DeleteObject(pb);
        }
        std::string label = std::string(tag) + " " + phase;
        TextOutA(mem, hx - 34, rail_y - 18, label.c_str(),
                 static_cast<int>(label.size()));
    };
    picker(s.load_x_mm,   s.load_z_mm,   s.load_carry,   70,
           RGB(90, 150, 230), "LD", s.load_phase);
    picker(s.unload_x_mm, s.unload_z_mm, s.unload_carry, 86,
           RGB(230, 130, 90), "UL", s.unload_phase);

    const char* hdr = "uniflow CNC line - LoadPicker / Stage / UnloadPicker, "
                      "orchestrator-driven, lock-free B-zone hand-off";
    TextOutA(mem, 50, 16, hdr, lstrlenA(hdr));
    std::string done = "delivered at C: " + std::to_string(s.delivered);
    TextOutA(mem, 760, 16, done.c_str(), static_cast<int>(done.size()));
    std::string stg = std::string("stage: ") + ToString(s.stage_state)
                      + " (" + s.stage_phase + ")";
    TextOutA(mem, 50, 36, stg.c_str(), static_cast<int>(stg.size()));

    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
        SetTimer(hwnd, 1, 16, nullptr);
        return 0;
    case WM_TIMER:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC         hdc = BeginPaint(hwnd, &ps);
        RECT        rc;
        GetClientRect(hwnd, &rc);
        DrawScene(hdc, rc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

static void RunVisualisation()
{
    const char* cls = "uniflow_cnc";
    WNDCLASSA   wc{};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandle(nullptr);
    wc.lpszClassName = cls;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowA(cls, "uniflow - CNC pickers",
                              WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1000, 470,
                              nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

#else // ---------- non-Windows: console animation -----------------------

static void RunVisualisation()
{
    auto cell = [](double x) { return static_cast<int>(x / LineGeometry::kXMax_mm * 64.0); };
    const auto t0 = std::chrono::steady_clock::now();
    std::cout << "uniflow CNC line (console). Runs ~20s.\n";
    while (std::chrono::steady_clock::now() - t0 < 20s)
    {
        Snapshot s = ReadSnapshot();
        std::string track(66, ' ');
        track[cell(LineGeometry::kZoneA_mm)] = 'A';
        track[cell(LineGeometry::kZoneB_mm)] = 'B';
        track[cell(LineGeometry::kZoneC_mm)] = 'C';
        std::string ld(66, ' '), ul(66, ' ');
        ld[cell(s.load_x_mm)]   = s.load_carry   ? '#' : 'v';
        ul[cell(s.unload_x_mm)] = s.unload_carry ? '#' : 'v';
        std::cout << "\r LD[" << ld << "] " << s.load_phase << "        \n"
                  << " UL[" << ul << "] " << s.unload_phase << "        \n"
                  << "   [" << track << "]  stage=" << ToString(s.stage_state)
                  << " delivered=" << s.delivered << "   \x1b[3A" << std::flush;
        std::this_thread::sleep_for(80ms);
    }
    std::cout << "\x1b[3B\n";
}

#endif

// ======================================================================
int main()
{
    // BS thread pool for the HW handshake simulations (SendStart / Cleanup).
    uniflow::RegisterExecutor(
        "default", std::make_shared<uniflow::BSThreadPoolExecutor>(2));

    // Console + file logging. The file is opened/closed on every line.
    uniflow::SetObserver(std::make_unique<LineLogObserver>("cnc_pickers.log"));

    // The two pickers are non-singleton modules - construct them here so
    // their names land in the registry before any flow looks them up.
    LoadPicker   load;
    UnloadPicker unload;

    StartLine();
    RunVisualisation();
    StopLine();

    std::cout << "parts delivered to Unload: "
              << LineState::DeliveredCount() << "\n";
    return 0;
}
