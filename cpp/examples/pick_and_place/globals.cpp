#include "globals.h"

#include <cmath>

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
    // Flow_Orchestrator polls every Config::stay_sleep (default 20ms) and
    // picks the new part up on its next round - no external wake needed.
}
void GlobalEnv::ConsumeZoneAPart() { g_zone_a_part = false; }

int  GlobalEnv::DeliveredCount() { return g_delivered; }
void GlobalEnv::IncDelivered()   { ++g_delivered; }

bool GlobalEnv::Stop()        { return g_stop.load(std::memory_order_relaxed); }
void GlobalEnv::RequestStop() { g_stop.store(true, std::memory_order_relaxed); }

// ----- StageState ------------------------------------------------------

const char* ToString(StageState s)
{
    switch (s)
    {
    case StageState::Idle:               return "Idle";
    case StageState::RawPartLoaded:      return "RawPartLoaded";
    case StageState::Prepared:           return "Prepared";
    case StageState::Machined:           return "Machined";
    case StageState::ProcessedPartReady: return "ProcessedPartReady";
    }
    return "?";
}
