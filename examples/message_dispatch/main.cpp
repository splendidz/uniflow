// message_dispatch - one student, two clients (professor + friend), and
// a shared mailbox. All four are Uniflow modules attached to the same
// Runtime; the mailbox is lock-free because every step runs on the same
// pump thread. The student drains the mailbox; each assignment goes
// through a train-or-sleep-then-do chain, each play goes through a
// play chain. SimHours() blocking is delegated to the pool via UF_ASYNC.
//
// A UF_Visualization module snapshots state every pump round, and main
// runs the Win32 message loop until the user closes the window.
#include "app.h"
#include "uf_visualization.h"

#include <iostream>

int main()
{
    std::cout << "=== message_dispatch: a student, two clients, one mailbox ==="
              << "\n\n";

    App& app = App::inst();
    app.Start();

    RunVisualisation();  // blocks until the window closes

    app.Shutdown();

    const UF_Student& s = app.student;
    std::cout << "\n=== done: hours=" << s.HoursSpent()
              << "  ability=" << s.Ability()
              << "  stress=" << s.Stress()
              << "  messages_handled=" << s.Done_count() << " ===\n";
    return 0;
}
