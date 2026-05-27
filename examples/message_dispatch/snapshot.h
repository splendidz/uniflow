// snapshot.h - the viz step on the pump thread writes into g_snap under
// g_snap_mu, and the Win32 paint loop on the main thread reads it under
// the same mutex. Same pattern as cnc_pickers.
#pragma once

#include "globals.h"

#include <mutex>
#include <string>
#include <vector>

struct Snapshot
{
    // mailbox snapshot
    std::vector<Message> queue;

    // professor
    int                  prof_emitted = 0;
    int                  prof_total   = 0;
    std::vector<Message> prof_pending;
    std::string          prof_phase;
    bool                 prof_idle    = true;

    // friend
    int                  friend_emitted = 0;
    int                  friend_total   = 0;
    std::vector<Message> friend_pending;
    std::string          friend_phase;
    bool                 friend_idle    = true;

    // student
    Message              student_current;
    int                  student_ability = 0;
    int                  student_stress  = 0;
    int                  student_hours   = 0;
    int                  student_done    = 0;
    std::string          student_phase;
    bool                 student_idle    = true;
};

extern std::mutex g_snap_mu;
extern Snapshot   g_snap;

Snapshot ReadSnapshot();
