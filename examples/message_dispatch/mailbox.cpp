#include "mailbox.h"

#include <deque>

namespace
{
    std::deque<Message> g_inbox;
}

void Mailbox::Push(const Message& m) { g_inbox.push_back(m); }

bool Mailbox::TryPop(Message& out)
{
    if (g_inbox.empty())
        return false;
    out = std::move(g_inbox.front());
    g_inbox.pop_front();
    return true;
}

std::size_t Mailbox::Size() { return g_inbox.size(); }

void Mailbox::ForEach(const std::function<void(const Message&)>& fn)
{
    for (const auto& m : g_inbox)
        fn(m);
}
