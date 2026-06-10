// snapshot.cpp - storage + locked read of the UI snapshot.
#include "snapshot.h"

Snapshot   g_snap;
std::mutex g_snap_mu;

Snapshot ReadSnapshot()
{
    std::lock_guard<std::mutex> lk(g_snap_mu);
    return g_snap;
}
