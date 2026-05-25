#include "globals.h"

#include "app.h"

#include <chrono>
#include <cmath>
#include <random>
#include <thread>

// ----- GlobalGeometry --------------------------------------------------

bool GlobalGeometry::InsideZoneB(double x_mm)
{
    return std::fabs(x_mm - kZoneB_mm) < kBSafetyGap_mm;
}

// ----- GlobalEnv -------------------------------------------------------

namespace {
    bool             g_zone_a_part = false;
    int              g_delivered   = 0;
    std::atomic<bool> g_stop{false};
}

bool GlobalEnv::ZoneAHasPart() { return g_zone_a_part; }
void GlobalEnv::CreateFakeZoneAPart()
{
    g_zone_a_part = true;
    App::inst().rt.Notify();   // wake Orchestrator without waiting kSchedTick
}
void GlobalEnv::ConsumeZoneAPart() { g_zone_a_part = false; }

int  GlobalEnv::DeliveredCount() { return g_delivered; }
void GlobalEnv::IncDelivered()   { ++g_delivered; }

bool GlobalEnv::Stop()        { return g_stop.load(std::memory_order_relaxed); }
void GlobalEnv::RequestStop() { g_stop.store(true, std::memory_order_relaxed); }

// ----- HwSimulator -----------------------------------------------------

namespace {
    std::atomic<bool> g_hw_ready{false};
}

void HwSimulator::DoReady()
{
    g_hw_ready.store(false, std::memory_order_release);
    std::thread([]
    {
        static thread_local std::mt19937 rng{std::random_device{}()};
        std::uniform_int_distribution<int> d(200, 700);
        int ms = d(rng);
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        g_hw_ready.store(true, std::memory_order_release);
        // No Notify() here on purpose: real HW would not know about uniflow.
        // Stage polls IsReady() at its own cadence.
    }).detach();
}
bool HwSimulator::IsReady()
{
    return g_hw_ready.load(std::memory_order_acquire);
}

// ----- StageState ------------------------------------------------------

const char* ToString(StageState s)
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

// ----- MotorAxis -------------------------------------------------------

MotorAxis::MotorAxis(double initial_mm)
    : pos_mm_(initial_mm),
      target_mm_(initial_mm),
      last_update_at_(uniflow::Clock::now())
{}

void MotorAxis::SetTarget(double target_mm)
{
    if (target_mm == target_mm_)
        return;
    target_mm_      = target_mm;
    // Restart the integration clock at the command instant. Without this,
    // a long gap with no Update() between two motions (e.g. picker doing a
    // 1.6 s X retreat between Z cycles) lets the next Update see a huge
    // dt and jump the axis instantly to target - making the visible motion
    // asymmetric between "lower" (long gap, looks teleported) and "lift"
    // (short gap, runs at the real speed).
    last_update_at_   = uniflow::Clock::now();
    in_window_since_.reset();
}

void MotorAxis::Update(double speed_mm_per_s)
{
    auto   now  = uniflow::Clock::now();
    double dt_s =
        std::chrono::duration<double>(now - last_update_at_).count();
    last_update_at_ = now;

    double remaining = target_mm_ - pos_mm_;
    double step      = speed_mm_per_s * dt_s;
    if (std::fabs(remaining) <= step)
        pos_mm_ = target_mm_;
    else
        pos_mm_ += (remaining > 0 ? step : -step);
}

bool MotorAxis::InPosition(double tol_mm, uniflow::Duration settle) const
{
    if (std::fabs(pos_mm_ - target_mm_) > tol_mm)
    {
        in_window_since_.reset();
        return false;
    }
    auto now = uniflow::Clock::now();
    if (!in_window_since_)
        in_window_since_ = now;
    return (now - *in_window_since_) >= settle;
}
