#include "console.h"

#include <iostream>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace console
{

const std::string kClearLine     = "\x1b[2K";
const std::string kSaveCursor    = "\x1b[s";
const std::string kRestoreCursor = "\x1b[u";

const char* kReset  = "\x1b[0m";
const char* kBold   = "\x1b[1m";
const char* kDim    = "\x1b[2m";
const char* kRed    = "\x1b[31m";
const char* kGreen  = "\x1b[32m";
const char* kYellow = "\x1b[33m";
const char* kCyan   = "\x1b[36m";
const char* kGray   = "\x1b[90m";

void EnableAnsi()
{
#if defined(_WIN32)
    // Opt the console into ANSI/VT escape handling. Without this, Windows
    // conhost prints the raw escape bytes instead of acting on them.
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out == INVALID_HANDLE_VALUE)
    {
        return;
    }
    DWORD mode = 0;
    if (!GetConsoleMode(out, &mode))
    {
        return;
    }
    SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#endif
    // POSIX terminals already interpret ANSI; nothing to do.
}

void HideCursor()
{
    std::cout << "\x1b[?25l" << std::flush;
}

void ShowCursor()
{
    std::cout << "\x1b[?25h" << std::flush;
}

void Clear()
{
    std::cout << "\x1b[2J\x1b[H" << std::flush;
}

void MoveHome()
{
    std::cout << "\x1b[H" << std::flush;
}

std::string At(int row, int col)
{
    return "\x1b[" + std::to_string(row) + ";" + std::to_string(col) + "H";
}

std::string Bar(double frac, int width, char fill, char empty)
{
    if (frac < 0.0)
    {
        frac = 0.0;
    }
    if (frac > 1.0)
    {
        frac = 1.0;
    }
    int filled = static_cast<int>(frac * width + 0.5);
    std::string s;
    s.reserve(static_cast<std::size_t>(width));
    for (int i = 0; i < width; ++i)
    {
        s.push_back(i < filled ? fill : empty);
    }
    return s;
}

std::string Fg(unsigned char r, unsigned char g, unsigned char b)
{
    return "\x1b[38;2;" + std::to_string(static_cast<int>(r)) + ";"
           + std::to_string(static_cast<int>(g)) + ";"
           + std::to_string(static_cast<int>(b)) + "m";
}

}  // namespace console
