// snapshot.h - the viz step on the pump thread writes into g_snap
// under g_snap_mu, and the Win32 paint loop on the main thread reads
// from it under the same mutex. Mirrors the cnc_pickers pattern.
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

    RecvState   recv_state   = RecvState::Idle;
    int         processed    = 0;
    std::string last_result;
    Msg         current{};

    std::string sender_phase;
    std::string recv_phase;
};

extern std::mutex g_snap_mu;
extern Snapshot   g_snap;

Snapshot ReadSnapshot();
