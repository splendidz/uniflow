// globals.cpp - backing storage for the simulation flags.
#include "globals.h"

#include <atomic>

namespace sim
{

namespace
{
std::atomic<bool> g_stop{false};
}

bool Stop() { return g_stop.load(std::memory_order_relaxed); }
void RequestStop() { g_stop.store(true, std::memory_order_relaxed); }

} // namespace sim
