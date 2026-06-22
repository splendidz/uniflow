// AnsiConsole.cs - tiny cross-platform ANSI terminal helper for the C# examples.
//
// The C# mirror of the examples' console helper (console.h / console.py):
// dependency-free ANSI escape sequences that work on Linux/macOS terminals and
// Windows Terminal / Windows 10+. EnableAnsi() turns on VT processing on Windows
// and is a no-op elsewhere.
using System;
using System.Runtime.InteropServices;

namespace Uniflow.Examples
{
    public static class Ansi
    {
        public const string ClearLine = "\x1b[2K";
        public const string SaveCursor = "\x1b[s";
        public const string RestoreCursor = "\x1b[u";

        public const string Reset = "\x1b[0m";
        public const string Bold = "\x1b[1m";
        public const string Dim = "\x1b[2m";
        public const string Red = "\x1b[31m";
        public const string Green = "\x1b[32m";
        public const string Yellow = "\x1b[33m";
        public const string Cyan = "\x1b[36m";
        public const string Gray = "\x1b[90m";

        // Turn on virtual-terminal (ANSI) processing. No-op off Windows.
        public static void EnableAnsi()
        {
            if (!OperatingSystem.IsWindows())
            {
                return;
            }
            try
            {
                IntPtr handle = GetStdHandle(-11); // STD_OUTPUT_HANDLE
                if (GetConsoleMode(handle, out uint mode))
                {
                    // ENABLE_VIRTUAL_TERMINAL_PROCESSING = 0x0004
                    SetConsoleMode(handle, mode | 0x0004);
                }
            }
            catch
            {
                // best effort; leave the console as-is
            }
        }

        public static void HideCursor()
        {
            Console.Write("\x1b[?25l");
        }

        public static void ShowCursor()
        {
            Console.Write("\x1b[?25h");
        }

        public static void Clear()
        {
            Console.Write("\x1b[2J\x1b[H");
        }

        // Cursor-move escape to (row, col), 1-based.
        public static string At(int row, int col)
        {
            return "\x1b[" + row + ";" + col + "H";
        }

        // Fixed-width progress bar like "##########.........." for frac in [0,1].
        public static string Bar(double frac, int width, char fill = '#', char empty = '.')
        {
            if (frac < 0.0)
            {
                frac = 0.0;
            }
            if (frac > 1.0)
            {
                frac = 1.0;
            }
            int filled = (int)(frac * width + 0.5);
            return new string(fill, filled) + new string(empty, width - filled);
        }

        // 24-bit (truecolor) foreground prefix: ESC[38;2;r;g;bm. Pair with Reset.
        public static string Fg(int r, int g, int b)
        {
            return "\x1b[38;2;" + r + ";" + g + ";" + b + "m";
        }

        [DllImport("kernel32.dll")]
        private static extern IntPtr GetStdHandle(int nStdHandle);

        [DllImport("kernel32.dll")]
        private static extern bool GetConsoleMode(IntPtr hConsoleHandle, out uint lpMode);

        [DllImport("kernel32.dll")]
        private static extern bool SetConsoleMode(IntPtr hConsoleHandle, uint dwMode);
    }
}
