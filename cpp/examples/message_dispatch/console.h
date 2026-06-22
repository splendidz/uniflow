// console.h - tiny cross-platform ANSI terminal helper.
//
// This is the REUSABLE console-render pattern for uniflow examples on Linux
// (and any VT-capable terminal, incl. Windows Terminal / Windows 10+ conhost).
// city_traffic and pick_and_place reuse the same primitives for their console
// renderer; only the frame composition differs.
//
// Everything here writes plain ANSI escape sequences (all ASCII bytes). No
// third-party dependency, no Qt, nothing to install. EnableAnsi() flips on VT
// processing on Windows and is a no-op elsewhere.
#pragma once

#include <string>

namespace console
{

// Turn on virtual-terminal (ANSI) processing. Call once at startup.
// No-op on platforms where the terminal already understands ANSI.
void EnableAnsi();

void HideCursor();
void ShowCursor();
void Clear();             // erase whole screen, cursor to home
void MoveHome();          // cursor to row 1, col 1

// --- escape-sequence builders (compose a full frame, then write once) ---

std::string At(int row, int col);   // move cursor to (row, col), 1-based

extern const std::string kClearLine;     // erase the current line
extern const std::string kSaveCursor;    // save cursor position
extern const std::string kRestoreCursor; // restore saved cursor position

// --- styling (ANSI SGR), all ASCII ---

extern const char* kReset;
extern const char* kBold;
extern const char* kDim;
extern const char* kRed;
extern const char* kGreen;
extern const char* kYellow;
extern const char* kCyan;
extern const char* kGray;

// Fixed-width progress bar like "##########.........." for frac in [0,1].
std::string Bar(double frac, int width, char fill = '#', char empty = '.');

// 24-bit (truecolor) foreground prefix: ESC[38;2;r;g;bm. Pair with kReset.
// Works on Linux terminals and Windows Terminal / Windows 10+ conhost.
std::string Fg(unsigned char r, unsigned char g, unsigned char b);

}  // namespace console
