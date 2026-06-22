"""console.py - tiny cross-platform ANSI terminal helper for the Python examples.

The Python mirror of the C++ examples' console.h: dependency-free ANSI escape
sequences that work on Linux/macOS terminals and Windows Terminal / Windows 10+.
enable_ansi() turns on VT processing on Windows and is a no-op elsewhere.
"""

import sys


def enable_ansi() -> None:
    """Turn on virtual-terminal (ANSI) processing. No-op off Windows."""
    if sys.platform == "win32":
        try:
            import ctypes

            kernel32 = ctypes.windll.kernel32
            handle = kernel32.GetStdHandle(-11)  # STD_OUTPUT_HANDLE
            mode = ctypes.c_uint32()
            if kernel32.GetConsoleMode(handle, ctypes.byref(mode)):
                # ENABLE_VIRTUAL_TERMINAL_PROCESSING = 0x0004
                kernel32.SetConsoleMode(handle, mode.value | 0x0004)
        except Exception:
            pass


def hide_cursor() -> None:
    sys.stdout.write("\x1b[?25l")
    sys.stdout.flush()


def show_cursor() -> None:
    sys.stdout.write("\x1b[?25h")
    sys.stdout.flush()


def clear() -> None:
    sys.stdout.write("\x1b[2J\x1b[H")
    sys.stdout.flush()


def at(row: int, col: int) -> str:
    """Cursor-move escape to (row, col), 1-based."""
    return f"\x1b[{row};{col}H"


CLEAR_LINE = "\x1b[2K"
SAVE_CURSOR = "\x1b[s"
RESTORE_CURSOR = "\x1b[u"

RESET = "\x1b[0m"
BOLD = "\x1b[1m"
DIM = "\x1b[2m"
RED = "\x1b[31m"
GREEN = "\x1b[32m"
YELLOW = "\x1b[33m"
CYAN = "\x1b[36m"
GRAY = "\x1b[90m"


def bar(frac: float, width: int, fill: str = "#", empty: str = ".") -> str:
    """Fixed-width progress bar like '##########..........' for frac in [0,1]."""
    frac = max(0.0, min(1.0, frac))
    filled = int(frac * width + 0.5)
    return fill * filled + empty * (width - filled)


def fg(r: int, g: int, b: int) -> str:
    """24-bit (truecolor) foreground prefix: ESC[38;2;r;g;bm. Pair with RESET."""
    return f"\x1b[38;2;{int(r)};{int(g)};{int(b)}m"
