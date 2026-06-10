#include "snapshot.h"

std::mutex g_snap_mu;
Snapshot   g_snap;

Snapshot ReadSnapshot()
{
    std::lock_guard<std::mutex> lk(g_snap_mu);
    return g_snap;
}
