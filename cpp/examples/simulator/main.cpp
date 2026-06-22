// simulator - a console sim where many flows share one pump and one logical
// clock. Type commands to drive time itself:
//   pause       freeze logical time  (runners stop, dashboard stays live)
//   resume      resume logical time
//   speed <n>   scale logical time   (n > 0; 0.5 = half, 4 = 4x)
//   quit        stop and exit
//
// FEATURE FOCUS: VirtualClock (scale / freeze) + single-thread cooperation.
// The pump, the five runners, and the renderer all live on one thread; this
// main thread does nothing but read stdin and poke the clock (thread-safe).
#include "app.h"
#include "console.h"
#include "snapshot.h"

#include <iostream>
#include <sstream>
#include <string>

namespace
{

// Draw the input prompt on its reserved row. The renderer never touches this
// row, so what the user types is not overwritten by frame redraws.
void DrawPrompt()
{
    std::cout << console::At(sim::kPromptRow, 1) << console::kClearLine << "  "
              << console::kBold << "Type a command and press Enter"
              << console::kReset << console::kDim
              << " (pause | resume | speed <n> | quit): " << console::kReset
              << std::flush;
}

void RunInputLoop(uniflow::Runtime& rt)
{
    std::string line;
    while (true)
    {
        DrawPrompt();
        if (!std::getline(std::cin, line))
        {
            break;   // EOF (e.g. piped input ended) - treat as quit
        }

        std::istringstream in(line);
        std::string        cmd;
        in >> cmd;

        if (cmd.empty())
        {
            continue;
        }
        if (cmd == "quit" || cmd == "exit" || cmd == "q")
        {
            break;
        }
        if (cmd == "pause")
        {
            rt.clock().Freeze();   // stop logical time for ALL runners at once
        }
        else if (cmd == "resume")
        {
            rt.clock().Resume();
        }
        else if (cmd == "speed")
        {
            double n = 0.0;
            if (in >> n && n > 0.0)
            {
                rt.clock().SetScale(n);   // one call rescales every flow's pace
            }
        }
        // Unknown commands are ignored; the dashboard keeps running.
    }
}

}  // namespace

int main()
{
    console::EnableAnsi();
    console::HideCursor();
    console::Clear();

    App& app = App::inst();
    app.Start();

    RunInputLoop(app.rt);

    app.Shutdown();

    console::ShowCursor();
    std::cout << console::At(sim::kPromptRow + 1, 1) << console::kClearLine
              << "  simulator stopped.\n";
    return 0;
}
