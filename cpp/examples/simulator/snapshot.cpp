#include "snapshot.h"

namespace sim
{

RunnerRow        g_rows[kRunnerCount];
std::atomic<bool> g_stop{false};

}  // namespace sim
