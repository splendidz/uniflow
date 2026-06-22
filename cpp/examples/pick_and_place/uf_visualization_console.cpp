// uf_visualization_console.cpp - ANSI console render back-end (any terminal).
// A side view of the line: a top rail the two pickers hang from, their arms
// descending by Z, zones A/B/C and the stage table on the track below. Used on
// Linux/macOS, and on Windows when UF_RENDER=console. Reads the same Snapshot
// the Win32 back-end reads.
#include "uf_visualization.h"

#include "console.h"
#include "globals.h"
#include "snapshot.h"
#include "uniflow.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{

constexpr int kCols       = 70;
constexpr int kMargin     = 4;
constexpr int kRailRow    = 0;       // canvas row of the rail
constexpr int kZRows      = 6;       // rows of vertical (Z) travel
constexpr int kTrackRow   = kRailRow + kZRows + 2;
constexpr int kRows       = kTrackRow + 2;
constexpr int kHeaderRows = 3;       // title lines above the canvas

struct Cell
{
    char        ch = ' ';
    std::string color;
};

// Map an X position in mm to a canvas column.
int ColX(double x_mm)
{
    double f = x_mm / GlobalGeometry::kXMax_mm;
    if (f < 0.0) f = 0.0;
    if (f > 1.0) f = 1.0;
    return kMargin + static_cast<int>(f * (kCols - 2 * kMargin) + 0.5);
}

void Put(std::vector<Cell>& cv, int r, int c, char ch, const std::string& col)
{
    if (r < 0 || r >= kRows || c < 0 || c >= kCols)
        return;
    Cell& cell = cv[static_cast<std::size_t>(r) * kCols + c];
    cell.ch    = ch;
    cell.color = col;
}

// Draw one picker: an arm of '|' from the rail down to its Z depth, with a
// gripper tip ('#' carrying a part, 'v' open) in the picker's colour.
void DrawPicker(std::vector<Cell>& cv, double x_mm, double z_mm, bool carry,
                const std::string& col)
{
    int c    = ColX(x_mm);
    double f = z_mm / GlobalGeometry::kZDown_mm; // 0 up .. 1 fully down
    if (f < 0.0) f = 0.0;
    if (f > 1.0) f = 1.0;
    int tip = kRailRow + static_cast<int>(f * kZRows + 0.5);
    for (int r = kRailRow; r < tip; ++r)
        Put(cv, r, c, '|', col);
    Put(cv, tip, c, carry ? '#' : 'v', col);
}

void DrawConsole(const Snapshot& s)
{
    std::vector<Cell> cv(static_cast<std::size_t>(kRows) * kCols);

    const std::string ld_col = console::Fg(90, 150, 230);  // load picker: blue
    const std::string ul_col = console::Fg(230, 130, 90);  // unload picker: orange

    // rail across the top
    for (int c = kMargin; c < kCols - kMargin; ++c)
        Put(cv, kRailRow, c, '=', console::kGray);

    // track across the bottom, with zone markers
    for (int c = kMargin; c < kCols - kMargin; ++c)
        Put(cv, kTrackRow, c, '=', console::kGray);
    auto mark = [&](double x_mm, char ch)
    {
        Put(cv, kTrackRow, ColX(x_mm), ch,
            std::string(console::kBold) + console::Fg(225, 230, 240));
    };
    mark(GlobalGeometry::kZoneA_mm, 'A');
    mark(GlobalGeometry::kZoneB_mm, 'B');
    mark(GlobalGeometry::kZoneC_mm, 'C');

    // raw part waiting in zone A
    if (s.zoneA_has_part)
        Put(cv, kTrackRow - 1, ColX(GlobalGeometry::kZoneA_mm), 'o',
            console::Fg(210, 180, 70));

    // stage table at B, coloured by machining state
    std::string stage_col =
        s.stage_state == StageState::ProcessedPartReady ? console::Fg(90, 180, 120)
        : s.stage_state == StageState::Idle             ? console::Fg(120, 124, 134)
                                                        : console::Fg(210, 130, 60);
    Put(cv, kTrackRow - 1, ColX(s.stage_table_x_mm), '#', stage_col);

    // the two pickers hanging from the rail
    DrawPicker(cv, s.load_x_mm, s.load_z_mm, s.load_carry, ld_col);
    DrawPicker(cv, s.unload_x_mm, s.unload_z_mm, s.unload_carry, ul_col);

    // compose one frame and write it in a single flush.
    std::string out;
    out += console::At(1, 1) + console::kClearLine + "  " + console::kBold
           + "uniflow pick & place  " + console::kReset + console::kDim + "v"
           + uniflow::kVersion + console::kReset;
    out += console::At(2, 1) + console::kClearLine + "  " + "stage: "
           + console::kCyan + ToString(s.stage_state) + console::kReset + " ("
           + s.stage_phase + ")   delivered: " + console::kBold
           + std::to_string(s.delivered) + console::kReset + "   " + ld_col
           + "LD" + console::kReset + "/" + ul_col + "UL" + console::kReset;
    out += console::At(3, 1) + console::kClearLine + "  " + console::kDim
           + "press Enter to quit" + console::kReset;

    for (int r = 0; r < kRows; ++r)
    {
        out += console::At(r + 1 + kHeaderRows, 1) + console::kClearLine;
        std::string cur;
        for (int c = 0; c < kCols; ++c)
        {
            const Cell& cell = cv[static_cast<std::size_t>(r) * kCols + c];
            if (cell.color != cur)
            {
                out += cell.color.empty() ? std::string(console::kReset)
                                          : cell.color;
                cur = cell.color;
            }
            out += cell.ch;
        }
        out += console::kReset;
    }

    std::cout << out << std::flush;
}

} // namespace

void RunVisualisationConsole()
{
    console::EnableAnsi();
    console::HideCursor();
    console::Clear();

    // Render on a background thread; the main thread blocks on stdin so a single
    // Enter quits. The render thread only READS the snapshot (mutex-guarded).
    std::atomic<bool> done{false};
    std::thread       render([&]
    {
        using namespace std::chrono_literals;
        while (!done.load() && !GlobalEnv::Stop())
        {
            DrawConsole(ReadSnapshot());
            std::this_thread::sleep_for(50ms);
        }
    });

    std::string line;
    std::getline(std::cin, line); // any Enter (or EOF) quits
    done.store(true);
    render.join();

    console::ShowCursor();
    std::cout << console::At(kRows + kHeaderRows + 2, 1) << console::kClearLine
              << "  pick_and_place stopped.\n";
}
