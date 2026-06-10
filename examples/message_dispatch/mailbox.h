// mailbox.h - shared inbox for the student. The two spawners (Professor
// and Friend) push messages; the student pops one at a time and drains.
// All three modules are attached to the same Runtime, so the mailbox
// only ever sees the pump thread and needs no locking.
#pragma once

#include "globals.h"

#include <cstddef>
#include <functional>

class Mailbox
{
public:
    static void        Push(const Message& m);
    static bool        TryPop(Message& out);
    static std::size_t Size();

    // Walk the inbox front-to-back without popping. Used by the viz step
    // (on the same pump thread) to snapshot the queue.
    static void ForEach(const std::function<void(const Message&)>& fn);

    Mailbox() = delete;
};
