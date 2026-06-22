#include "globals.h"

#include <atomic>
#include <deque>

namespace
{
    std::atomic<bool>       g_stop{false};

    std::mutex              g_log_mu;
    std::deque<std::string> g_log;
    constexpr std::size_t   kLogCap = 64;
}

bool GlobalEnv::Stop()        { return g_stop.load(); }
void GlobalEnv::RequestStop() { g_stop.store(true); }

void GlobalLog::Add(const std::string& line)
{
    std::lock_guard<std::mutex> lk(g_log_mu);
    g_log.push_back(line);
    while (g_log.size() > kLogCap)
    {
        g_log.pop_front();
    }
}

std::vector<std::string> GlobalLog::Recent(std::size_t n)
{
    std::lock_guard<std::mutex> lk(g_log_mu);
    std::vector<std::string> out;
    std::size_t start = g_log.size() > n ? g_log.size() - n : 0;
    for (std::size_t i = start; i < g_log.size(); ++i)
    {
        out.push_back(g_log[i]);
    }
    return out;
}
