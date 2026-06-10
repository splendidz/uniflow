#include "globals.h"

#include <atomic>

namespace
{
    std::atomic<bool> g_stop{false};
}

bool GlobalEnv::Stop()        { return g_stop.load(); }
void GlobalEnv::RequestStop() { g_stop.store(true); }
