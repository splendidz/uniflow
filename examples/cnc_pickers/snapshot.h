// ======================================================================
//  snapshot.h - the one cross-thread hand-off (pump -> UI thread).
//
//  Viz fills g_snap under g_snap_mu every kVizTick; the UI/render thread
//  calls ReadSnapshot() to get a consistent copy. No other lock crosses
//  the pump/UI boundary in this example.
// ======================================================================
#pragma once

#include "globals.h"

#include <mutex>
#include <string>

struct Snapshot
{
    double      load_x_mm           = GlobalGeometry::kZoneA_mm;
    double      load_z_mm           = GlobalGeometry::kZUp_mm;
    bool        load_carry          = false;
    double      load_finger_gap_mm  = GlobalGeometry::kFingerOpen_mm;
    std::string load_phase          = "-";

    double      unload_x_mm           = GlobalGeometry::kZoneC_mm;
    double      unload_z_mm           = GlobalGeometry::kZUp_mm;
    bool        unload_carry          = false;
    double      unload_finger_gap_mm  = GlobalGeometry::kFingerOpen_mm;
    std::string unload_phase          = "-";

    double      stage_table_x_mm = GlobalGeometry::kZoneB_mm;
    double      stage_table_y_mm = 0.0;
    StageState  stage_state      = StageState::Idle;
    std::string stage_phase      = "-";

    bool zoneA_has_part = false;
    int  delivered      = 0;
};

extern Snapshot   g_snap;
extern std::mutex g_snap_mu;

Snapshot ReadSnapshot();
