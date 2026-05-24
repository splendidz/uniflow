// ======================================================================
//  globals.h - dimensional constants, timing constants, line-level state,
//  and the small simulation utilities (HwSimulator, MotorAxis, StageState).
//
//  These are the things every module agrees on but no single module owns.
//  Putting them in one header keeps "what physically is the line?" in
//  exactly one place, and lets the per-module headers stay focused on
//  THIS module's logic without hauling in every other type.
//
//  Definitions for the non-constexpr members live in globals.cpp.
// ======================================================================
#pragma once

#include "uniflow.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>

using namespace std::chrono_literals;

// ======================================================================
//  GlobalGeometry - everything dimensional about the line.
// ======================================================================
class GlobalGeometry
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
    static constexpr double kXSpeed_mm_per_s = 300.0;
    static constexpr double kZSpeed_mm_per_s = 200.0;

    // Two-finger gripper.
    static constexpr double kFingerOpen_mm        = 24.0;
    static constexpr double kPartWidth_mm         = 20.0;
    static constexpr double kFingerSpeed_mm_per_s = 200.0;

    // Tolerance + settle time for the InPosition predicate.
    static constexpr double                kPosTolerance_mm = 0.5;
    static constexpr uniflow::Duration     kPosSettleTime   = 80ms;

    static bool InsideZoneB(double x_mm);

    GlobalGeometry() = delete;
};

// ======================================================================
//  GlobalTiming - scheduling cadence (how often modules poll / re-tick).
// ======================================================================
class GlobalTiming
{
public:
    static constexpr uniflow::Duration kPoll_fast    = 10ms;
    static constexpr uniflow::Duration kPoll_slow    = 50ms;
    static constexpr uniflow::Duration kVizTick      = 16ms;
    static constexpr uniflow::Duration kSchedTick    = 30ms;

    static constexpr uniflow::Duration kLoaderMinGap = 1000ms;
    static constexpr uniflow::Duration kLoaderMaxGap = 5000ms;

    GlobalTiming() = delete;
};

// ======================================================================
//  GlobalEnv - line-level state several modules touch.
//
//  Mutators that change scheduling-relevant state call uniflow::NotifyAll()
//  so the Orchestrator wakes immediately - the per-tick poll exists only
//  as a backstop for time-driven decisions (e.g. raw-part creation).
// ======================================================================
class GlobalEnv
{
public:
    static bool ZoneAHasPart();
    static void CreateFakeZoneAPart();
    static void ConsumeZoneAPart();

    static int  DeliveredCount();
    static void IncDelivered();

    static bool Stop();
    static void RequestStop();

    GlobalEnv() = delete;
};

// ======================================================================
//  HwSimulator - fakes the machining HW's "ready" handshake.
//
//  Stage calls DoReady() right after the start-command ack. A detached
//  worker thread sleeps a random 500-2500 ms and then flips IsReady() to
//  true. Real machining HW would not know that "uniflow" exists, so the
//  simulator does not call uniflow::NotifyAll() either - the module step
//  that cares about IsReady() polls at its own cadence.
// ======================================================================
class HwSimulator
{
public:
    static void DoReady();
    static bool IsReady();

    HwSimulator() = delete;
};

// ======================================================================
//  StageState - what the Stage thinks it is doing right now.
// ======================================================================
enum class StageState : uint8_t
{
    Idle,
    RawPartLoaded,
    Processing,
    ProcessedPartReady,
};

const char* ToString(StageState s);

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
    explicit MotorAxis(double initial_mm);

    void   SetTarget(double target_mm);
    void   Update(double speed_mm_per_s);
    bool   InPosition(double tol_mm    = GlobalGeometry::kPosTolerance_mm,
                      uniflow::Duration settle = GlobalGeometry::kPosSettleTime) const;

    double Position() const { return pos_mm_; }
    double Target()   const { return target_mm_; }

private:
    double                                       pos_mm_;
    double                                       target_mm_;
    uniflow::TimePoint                           last_update_at_;
    mutable std::optional<uniflow::TimePoint>    in_window_since_;
};
