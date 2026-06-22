// uf_visualization_console.cpp - ANSI console render back-end (any terminal).
// Rasterises the grid map, per-approach signal lamps, and the moving fleet onto
// a character canvas with 24-bit colour. Used on Linux/macOS, and on Windows
// when UF_RENDER=console. Reads the same Snapshot the Win32 back-end reads.
#include "uf_visualization.h"

#include "console.h"
#include "globals.h"
#include "map.h"
#include "snapshot.h"
#include "uniflow.hpp"
#include "world.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{

// Character-canvas resolution. Cols-per-grid-unit is ~2x rows-per-grid-unit so
// the square map looks square despite terminal cells being ~2:1 tall.
constexpr int kSX   = 24; // columns per grid unit
constexpr int kSY   = 10; // rows per grid unit
constexpr int kCols = 2 * kSX + 1;  // grid spans [0,2]
constexpr int kRows = 2 * kSY + 1;
constexpr int kHeaderRows = 3;      // title lines above the map

struct Cell
{
    char        ch    = ' ';
    std::string color;            // ANSI SGR prefix; empty = default
};

int Col(double gx) { return static_cast<int>(std::lround(gx * kSX)); }
int Row(double gy) { return static_cast<int>(std::lround(gy * kSY)); }

// Signal-lamp colour.
std::string LampColor(city::LightColor c)
{
    switch (c)
    {
    case city::LightColor::Green:  return console::Fg(60, 220, 100);
    case city::LightColor::Yellow: return console::Fg(240, 200, 60);
    default:                       return console::Fg(170, 70, 70); // dim red
    }
}

void Put(std::vector<Cell>& canvas, int r, int c, char ch, const std::string& col)
{
    if (r < 0 || r >= kRows || c < 0 || c >= kCols)
        return;
    Cell& cell = canvas[static_cast<std::size_t>(r) * kCols + c];
    cell.ch    = ch;
    cell.color = col;
}

void DrawConsole(const Snapshot& s)
{
    std::vector<Cell> canvas(static_cast<std::size_t>(kRows) * kCols);

    const std::string road = console::kGray;

    // 1) roads: a dim line of '.' between each pair of connected nodes.
    for (const citymap::Edge& e : citymap::Edges())
    {
        const citymap::Node& a = citymap::NodeById(e.a);
        const citymap::Node& b = citymap::NodeById(e.b);
        int ca = Col(a.gx), ra = Row(a.gy);
        int cb = Col(b.gx), rb = Row(b.gy);
        if (ra == rb) // horizontal road
        {
            for (int c = std::min(ca, cb); c <= std::max(ca, cb); ++c)
                Put(canvas, ra, c, '.', road);
        }
        else // vertical road
        {
            for (int r = std::min(ra, rb); r <= std::max(ra, rb); ++r)
                Put(canvas, r, ca, '.', road);
        }
    }

    // 2) signal lamps: one 'O' per approach, placed a few cells out from the
    //    junction toward the approaching road, lit by that approach's signal.
    for (const citymap::Node& n : citymap::Nodes())
    {
        if (n.kind == citymap::NodeKind::Corner)
            continue;
        if (n.id >= static_cast<int>(s.signals.size()))
            continue;
        const city::SignalState& st = s.signals[n.id];
        int nc = Col(n.gx), nr = Row(n.gy);
        for (int nb : citymap::NeighborsOf(n.id))
        {
            const citymap::Node& m = citymap::NodeById(nb);
            int dc = (m.gx > n.gx) - (m.gx < n.gx); // -1 / 0 / +1
            int dr = (m.gy > n.gy) - (m.gy < n.gy);
            city::Axis  ax  = city::AxisOfEdge(n, m);
            std::string col = LampColor(city::StraightLight(st, ax));
            Put(canvas, nr + dr * 2, nc + dc * 3, 'O', col);
        }
    }

    // 3) nodes: junctions show their label initial (X / T), corners a '+'.
    for (const citymap::Node& n : citymap::Nodes())
    {
        char ch = (n.kind == citymap::NodeKind::Corner)
                      ? '+'
                      : (n.label[0] ? n.label[0] : '#');
        Put(canvas, Row(n.gy), Col(n.gx), ch,
            std::string(console::kBold) + console::Fg(225, 230, 240));
    }

    // 4) vehicles (drawn last, on top): an arrow in the car's own colour,
    //    pointing the way it is heading.
    for (const VehicleView& v : s.vehicles)
    {
        char arrow;
        if (std::fabs(v.dx) >= std::fabs(v.dy))
            arrow = v.dx >= 0 ? '>' : '<';
        else
            arrow = v.dy >= 0 ? 'v' : '^'; // grid y grows downward
        Put(canvas, Row(v.gy), Col(v.gx), arrow, console::Fg(v.r, v.g, v.b));
    }

    // compose one frame and write it in a single flush to avoid flicker.
    std::string out;
    out += console::At(1, 1);
    out += console::kClearLine;
    out += std::string("  ") + console::kBold + "uniflow city traffic  "
           + console::kReset + console::kDim + "v" + uniflow::kVersion
           + console::kReset;
    out += console::At(2, 1) + console::kClearLine + "  " + console::kGray
           + "one pump thread: signals, acceleration, car-ahead spacing, "
             "intersection yielding"
           + console::kReset;
    out += console::At(3, 1) + console::kClearLine + "  " + console::kDim
           + "press Enter to quit" + console::kReset;

    for (int r = 0; r < kRows; ++r)
    {
        out += console::At(r + 1 + kHeaderRows, 1) + console::kClearLine;
        std::string cur; // current active colour to minimise escape spam
        for (int c = 0; c < kCols; ++c)
        {
            const Cell& cell = canvas[static_cast<std::size_t>(r) * kCols + c];
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
        while (!done.load() && !sim::Stop())
        {
            DrawConsole(ReadSnapshot());
            std::this_thread::sleep_for(33ms);
        }
    });

    std::string line;
    std::getline(std::cin, line); // any Enter (or EOF) quits
    done.store(true);
    render.join();

    console::ShowCursor();
    std::cout << console::At(kRows + kHeaderRows + 2, 1) << console::kClearLine
              << "  city_traffic stopped.\n";
}
