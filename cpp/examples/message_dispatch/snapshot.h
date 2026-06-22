// snapshot.h - the data the renderer draws each frame.
//
// The viz task on the pump thread writes g_snap under g_snap_mu every pump
// round; the render thread reads a copy under the same mutex. Cross-thread, so
// it carries a mutex (unlike simulator, where the renderer is itself a flow on
// the pump and needs none).
#pragma once

#include "globals.h"

#include <mutex>
#include <string>
#include <vector>

struct Snapshot
{
    // mailbox snapshot (front-to-back, not popped)
    std::vector<Message> queue;

    // professor
    int                  prof_emitted = 0;
    int                  prof_total   = 0;
    std::string          prof_phase;
    bool                 prof_idle    = true;

    // friend
    int                  friend_emitted = 0;
    int                  friend_total   = 0;
    std::string          friend_phase;
    bool                 friend_idle    = true;

    // student
    Message              student_current;
    bool                 student_has_msg = false;
    int                  student_ability = 0;
    int                  student_stress  = 0;
    int                  student_hours   = 0;
    int                  student_done    = 0;
    std::string          student_phase;
    bool                 student_idle    = true;

    // recent activity (most recent last)
    std::vector<std::string> recent;
};

extern std::mutex g_snap_mu;
extern Snapshot   g_snap;

Snapshot ReadSnapshot();
