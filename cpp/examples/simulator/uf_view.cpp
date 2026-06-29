#include "uf_view.h"

#include "console.h"
#include "snapshot.h"

#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

using namespace uniflow;
using namespace std::chrono_literals;

namespace
{
const std::string kSep = "  " + std::string(60, '-');
}

Flow_View::Flow_View(uniflow::Runtime& rt)
    : uniflow::Uniflow<Flow_View>(rt, "Flow_View"),
      clock_(rt.clock())
{
    AddTask(task_draw_);
}

StepResult Flow_View::Task_Draw::Step1_Draw()
{
    if (sim::g_stop.load())
    {
        return Done();
    }
    // Throttle to ~30 fps on REAL time. fps_ is a default UFTimer (wall clock),
    // so the dashboard keeps refreshing even while the sim clock is frozen.
    if (fps_.Passed(33ms))
    {
        fps_.Restart();
        Render();
    }
    return Stay();
}

void Flow_View::Task_Draw::Render()
{
    std::ostringstream out;

    // Save the user's cursor (sitting on the prompt line), redraw the dashboard
    // above it at fixed positions, then restore it. The prompt is never touched.
    out << console::kSaveCursor;

    auto put = [&](int row, const std::string& text)
    {
        out << console::At(row, 1) << console::kClearLine << text;
    };

    put(1, std::string("  ") + console::kBold + "uniflow simulator  " + console::kReset
               + console::kDim + "v" + uniflow::kVersion + console::kReset);
    put(2, kSep);

    // Header reads live scale/freeze straight off the VirtualClock.
    std::ostringstream status;
    status << "  ";
    if (flow().clock_.Frozen())
    {
        status << console::kYellow << "[PAUSED] " << console::kReset;
    }
    else
    {
        status << console::kGreen << "[RUNNING]" << console::kReset;
    }
    status << "   speed " << console::kCyan << "x" << std::fixed << std::setprecision(2)
           << flow().clock_.Scale() << console::kReset
           << "      " << console::kGray
           << "pause | resume | speed <n> | quit" << console::kReset;
    put(3, status.str());
    put(4, kSep);

    for (int i = 0; i < sim::kRunnerCount; ++i)
    {
        const sim::RunnerRow& r = sim::g_rows[i];
        std::ostringstream line;
        line << "  " << std::left << std::setw(8) << r.name
             << " lap " << std::right << std::setw(2) << r.lap
             << "  [" << console::kGreen << console::Bar(r.percent / 100.0, 20)
             << console::kReset << "] " << std::right << std::setw(3)
             << static_cast<int>(r.percent + 0.5) << "%  "
             << console::kDim << r.step << console::kReset;
        put(5 + i, line.str());
    }

    put(5 + sim::kRunnerCount, kSep);

    out << console::kRestoreCursor;
    std::cout << out.str() << std::flush;
}
