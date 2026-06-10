// queue_drain - sender pushes random arithmetic jobs into a mailbox in
// bursts (1-10 at a time); receiver drains the mailbox one job at a
// time, dispatching to its own Add / Sub routines. Both modules run
// on the same Runtime, so the mailbox is lock-free.
//
// The Win32 visualiser shows the source vectors, the current queue,
// and the receiver state.
#include "app.h"
#include "uf_visualization.h"

#include <iostream>

int main()
{
    std::cout << "=== queue_drain: sender bursts, receiver drains ==="
              << "\n\n";

    App& app = App::inst();
    app.Start();

    RunVisualisation();

    app.Shutdown();
    return 0;
}
