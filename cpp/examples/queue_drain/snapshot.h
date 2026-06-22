// snapshot.h - one frame of state for the renderer.
//
// The Flow_Visualization snapshot step (pump thread) writes g_snap under
// g_snap_mu; the background render thread reads it under the same mutex. That
// mutex is the demo's ONLY cross-thread synchronisation - the mailbox itself is
// lock-free because the sender and receiver share the pump thread.
#pragma once

#include "globals.h"
#include "uf_receiver.h"

#include <mutex>
#include <string>
#include <vector>

struct Snapshot
{
    std::vector<int> vec_a;
    std::vector<int> vec_b;
    std::vector<Msg> queue;
    int              last_burst_count = 0;
    int              total_bursts     = 0;

    RecvState   recv_state = RecvState::Idle;
    int         processed  = 0;
    std::string last_result;
    Msg         current{};

    // Throughput metrics so the per-job cycle speed is visible on the dashboard.
    double last_cycle_ms  = 0.0;  // wall time the most recent job took
    double jobs_per_sec   = 0.0;  // instantaneous rate (1000 / last_cycle_ms)
    double avg_ms_per_job = 0.0;  // overall average over the whole run

    std::string sender_phase;
    std::string recv_phase;
};

extern std::mutex g_snap_mu;
extern Snapshot   g_snap;

Snapshot ReadSnapshot();
