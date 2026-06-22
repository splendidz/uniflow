// message_dispatch - one student, two spawners (professor + friend), and a
// shared mailbox. All four are uniflow modules attached to the SAME Runtime;
// the mailbox is lock-free because every step runs on the same pump thread.
// The student drains the mailbox one message at a time and routes by
// Message::Kind: an Assignment goes through a train/sleep/work chain, a Play
// through a play chain. The blocking "SimHours" work is offloaded to the pool
// with SubmitAsync and polled in a later step with AsyncResult<int>(id).
//
// CONSOLE MODE: a Flow_Visualization task snapshots the world each pump round;
// a background render thread paints an ANSI dashboard ~25 fps while the main
// thread blocks on std::getline (Enter to quit), exactly like the simulator and
// city_traffic console renderers. The renderer owns stdout, so the Runtime uses
// a SilentObserver to keep step-trace output off the screen.
#include "app.h"
#include "console.h"
#include "uf_visualization.h"

#include <atomic>
#include <iostream>
#include <string>
#include <thread>

int main()
{
    console::EnableAnsi();
    console::HideCursor();
    console::Clear();

    App& app = App::inst();
    app.Start();

    // Render thread paints; main thread blocks on Enter.
    std::atomic<bool> quit{false};
    std::thread       renderer(RunConsoleRenderer, std::ref(quit));

    std::string line;
    std::getline(std::cin, line);   // Enter (or EOF) quits

    quit.store(true);
    renderer.join();

    app.Shutdown();

    console::ShowCursor();
    const Flow_Student& s = app.student;
    std::cout << console::At(40, 1) << console::kClearLine
              << "message_dispatch stopped:  hours=" << s.HoursSpent()
              << "  ability=" << s.Ability()
              << "  stress=" << s.Stress()
              << "  messages_handled=" << s.DoneCount() << "\n";
    return 0;
}
