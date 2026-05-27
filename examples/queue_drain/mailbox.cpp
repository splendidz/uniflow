#include "mailbox.h"

namespace
{
    std::deque<Msg> g_queue;
}

void Mailbox::Push(const Msg& m) { g_queue.push_back(m); }

bool Mailbox::TryPop(Msg& out)
{
    if (g_queue.empty())
        return false;
    out = g_queue.front();
    g_queue.pop_front();
    return true;
}

std::size_t Mailbox::Size() { return g_queue.size(); }

std::vector<Msg> Mailbox::Snapshot()
{
    return std::vector<Msg>(g_queue.begin(), g_queue.end());
}
