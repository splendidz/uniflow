// mailbox.h - FIFO that the sender enqueues into and the receiver
// drains. Sender + receiver both live on the same Runtime, so the
// mailbox is touched only by one pump thread; the mutex exists only
// because the viz step (also on the pump thread) and snapshots are
// read on the main thread elsewhere.
#pragma once

#include "globals.h"

#include <deque>
#include <vector>

class Mailbox
{
public:
    static void Push(const Msg& m);
    static bool TryPop(Msg& out);
    static std::size_t Size();

    // Snapshot the queued items for the visualisation. Called on the
    // pump thread; the caller copies the result into g_snap.
    static std::vector<Msg> Snapshot();

    Mailbox() = delete;
};
