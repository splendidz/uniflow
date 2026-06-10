// globals.h - dimensional constants, timing, line-level state, sim utilities.
#pragma once

#include "uniflow.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <optional>

using namespace std::chrono_literals;

class GlobalGeometry
{
public:
    static constexpr double kZoneA_mm        = 200.0;
    static constexpr double kZoneB_mm        = 700.0;
    static constexpr double kZoneC_mm        = 1200.0;
    static constexpr double kXMax_mm         = 1400.0;
    static constexpr double kBSafetyGap_mm   = 250.0;
    static constexpr double kStageTravel_mm  = 80.0;

    static constexpr double kZUp_mm          = 0.0;
    static constexpr double kZDown_mm        = 120.0;  // down is positive

    static constexpr double kXSpeed_mm_per_s = 300.0;
    static constexpr double kZSpeed_mm_per_s = 200.0;

    static constexpr double kFingerOpen_mm        = 24.0;
    static constexpr double kPartWidth_mm         = 20.0;
    static constexpr double kFingerSpeed_mm_per_s = 200.0;

    static constexpr double            kPosTolerance_mm = 0.5;
    static constexpr uniflow::Duration kPosSettleTime   = 80ms;

    static bool InsideZoneB(double x_mm);

    GlobalGeometry() = delete;
};

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

class HwSimulator
{
public:
    static void DoReady();
    static bool IsReady();

    HwSimulator() = delete;
};

enum class StageState : uint8_t
{
    Idle,
    RawPartLoaded,
    Processing,
    ProcessedPartReady,
};

const char* ToString(StageState s);

// 1D linear axis with feedback. InPosition() needs the position held within
// tolerance for kPosSettleTime - matches real position-loop settle behaviour.
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
