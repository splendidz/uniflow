// queue_drain - a sender module pushes random arithmetic jobs into a shared
// mailbox in bursts; a receiver module drains the mailbox one job at a time,
// dispatching by operator. Both run on ONE pump thread, so the mailbox needs no
// lock - a lock-free single-pump producer / consumer.
//
// FEATURE FOCUS: lock-free producer / consumer on one pump thread; the receiver
// parks when the queue empties and the sender relaunches its drain task on the
// next burst (the IsIdle() + StartFlow() wake pattern).
#include "app.h"
#include "snapshot.h"
#include "uf_visualization.h"

#include <iostream>

int main()
{
    App& app = App::inst();   // Phase 1: every module is now constructed.
    app.Start();              // Phase 2: flows start; cross-module refs safe.

    RunVisualisation();       // main-thread render loop (background draw + stdin).

    app.Shutdown();

    Snapshot s = ReadSnapshot();
    std::cout << "  bursts sent: " << s.total_bursts
              << "   jobs processed: " << s.processed << "\n";
    return 0;
}
