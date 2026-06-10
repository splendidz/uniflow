#include "uf_visualization.h"

#include "app.h"
#include "mailbox.h"
#include "snapshot.h"
#include "uf_friend.h"
#include "uf_professor.h"
#include "uf_student.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

using namespace std::chrono_literals;

UF_Visualization::StepResult UF_Visualization::OnViz_Begin()
{
    Describe("viz running");
    return UF_NEXT(OnViz_Tick);
}

UF_Visualization::StepResult UF_Visualization::OnViz_Tick()
{
    if (GlobalEnv::Stop())
    {
        Describe("viz stop");
        return Done();
    }

    const auto& prof    = App::inst().prof;
    const auto& friend_ = App::inst().friend_;
    const auto& stu     = App::inst().student;

    Snapshot s;
    s.prof_emitted = prof.Emitted();
    s.prof_total   = prof.Total();
    s.prof_idle    = prof.IsIdle();
    s.prof_phase   = prof.CurrentStepDescription();
    {
        const auto& all = prof.Tasks();
        for (int i = s.prof_emitted; i < static_cast<int>(all.size()); ++i)
            s.prof_pending.push_back(all[i]);
    }

    s.friend_emitted = friend_.Emitted();
    s.friend_total   = friend_.Total();
    s.friend_idle    = friend_.IsIdle();
    s.friend_phase   = friend_.CurrentStepDescription();
    {
        const auto& all = friend_.Plays();
        for (int i = s.friend_emitted; i < static_cast<int>(all.size()); ++i)
            s.friend_pending.push_back(all[i]);
    }

    s.student_current = stu.CurrentMessage();
    s.student_ability = stu.Ability();
    s.student_stress  = stu.Stress();
    s.student_hours   = stu.HoursSpent();
    s.student_done    = stu.Done_count();
    s.student_idle    = stu.IsIdle();
    s.student_phase   = stu.CurrentStepDescription();

    // Drain a snapshot of the mailbox without popping. Mailbox lives on the
    // pump thread - safe to walk it directly here (viz step is on the pump
    // thread too).
    s.queue.reserve(Mailbox::Size());
    Mailbox::ForEach([&](const Message& m){ s.queue.push_back(m); });

    {
        std::lock_guard<std::mutex> lk(g_snap_mu);
        g_snap = std::move(s);
    }
    return Stay();
}

#if defined(_WIN32) && !defined(UNIFLOW_CONSOLE_VIZ)
#define UNIFLOW_WIN32_VIZ 1
#else
#define UNIFLOW_WIN32_VIZ 0
#endif

#if UNIFLOW_WIN32_VIZ
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace {

// ----- colour palette -----
constexpr COLORREF kBg        = RGB(22, 24, 30);
constexpr COLORREF kPanel     = RGB(32, 36, 44);
constexpr COLORREF kPanelEdge = RGB(58, 64, 76);
constexpr COLORREF kText      = RGB(220, 224, 232);
constexpr COLORREF kMuted     = RGB(140, 148, 160);
constexpr COLORREF kAccent    = RGB(120, 170, 240);
constexpr COLORREF kAssign    = RGB(195, 145, 60);   // amber
constexpr COLORREF kAssignBg  = RGB(58, 44, 26);
constexpr COLORREF kPlay      = RGB(110, 190, 120);  // green
constexpr COLORREF kPlayBg    = RGB(28, 52, 34);
constexpr COLORREF kAbility   = RGB(80, 150, 230);
constexpr COLORREF kStressLo  = RGB(110, 170, 110);
constexpr COLORREF kStressHi  = RGB(220, 100, 90);
constexpr COLORREF kBarTrack  = RGB(48, 52, 60);

HFONT g_font_title = nullptr;
HFONT g_font_h     = nullptr;
HFONT g_font_body  = nullptr;
HFONT g_font_small = nullptr;

void EnsureFonts()
{
    if (g_font_title) return;
    auto make = [](int height, int weight)
    {
        return CreateFontA(height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                           DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, "Segoe UI");
    };
    g_font_title = make(-20, FW_SEMIBOLD);
    g_font_h     = make(-15, FW_SEMIBOLD);
    g_font_body  = make(-13, FW_NORMAL);
    g_font_small = make(-12, FW_NORMAL);
}

void FillRectColor(HDC hdc, RECT r, COLORREF c)
{
    HBRUSH b = CreateSolidBrush(c);
    FillRect(hdc, &r, b);
    DeleteObject(b);
}

void StrokeRect(HDC hdc, RECT r, COLORREF c)
{
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HPEN old = static_cast<HPEN>(SelectObject(hdc, pen));
    HBRUSH oldb = static_cast<HBRUSH>(SelectObject(hdc, GetStockObject(NULL_BRUSH)));
    Rectangle(hdc, r.left, r.top, r.right, r.bottom);
    SelectObject(hdc, old);
    SelectObject(hdc, oldb);
    DeleteObject(pen);
}

// Rounded-corner-ish look using a 1px border drawn around a solid fill.
void DrawPanel(HDC hdc, RECT r, const char* title)
{
    FillRectColor(hdc, r, kPanel);
    StrokeRect(hdc, r, kPanelEdge);
    if (title && *title)
    {
        SelectObject(hdc, g_font_h);
        SetTextColor(hdc, kText);
        TextOutA(hdc, r.left + 14, r.top + 8, title, lstrlenA(title));
    }
}

void DrawChip(HDC hdc, int x, int y, int w, int h, COLORREF bg,
              COLORREF border, COLORREF fg, const std::string& text)
{
    RECT r{x, y, x + w, y + h};
    FillRectColor(hdc, r, bg);
    StrokeRect(hdc, r, border);
    SelectObject(hdc, g_font_body);
    SetTextColor(hdc, fg);
    TextOutA(hdc, x + 8, y + (h - 14) / 2,
             text.c_str(), static_cast<int>(text.size()));
}

void DrawBar(HDC hdc, int x, int y, int w, int h, int value, int maxv,
             COLORREF fill, const char* label)
{
    int filled = maxv > 0 ? (value * w) / maxv : 0;
    filled = (std::max)(0, (std::min)(w, filled));
    RECT bg_r{x, y, x + w, y + h};
    FillRectColor(hdc, bg_r, kBarTrack);
    StrokeRect(hdc, bg_r, kPanelEdge);
    if (filled > 0)
    {
        RECT fg_r{x, y, x + filled, y + h};
        FillRectColor(hdc, fg_r, fill);
    }
    SelectObject(hdc, g_font_small);
    SetTextColor(hdc, kText);
    std::string lab = std::string(label) + "  " + std::to_string(value)
                    + " / " + std::to_string(maxv);
    TextOutA(hdc, x, y - 18, lab.c_str(), static_cast<int>(lab.size()));
}

std::string FmtAssign(const Message& m)
{
    std::ostringstream os;
    os << "ASN  " << m.name
       << "  need_ability=" << m.need_ability
       << "  need_time=" << m.need_time << "h";
    return os.str();
}

std::string FmtPlay(const Message& m)
{
    std::ostringstream os;
    os << "PLAY " << m.name << "  " << m.play_hours << "h";
    return os.str();
}

std::string FmtMsg(const Message& m)
{
    return m.kind == Message::Kind::Assignment ? FmtAssign(m) : FmtPlay(m);
}

void DrawChipForMessage(HDC hdc, int x, int y, int w, int h, const Message& m)
{
    bool isA = (m.kind == Message::Kind::Assignment);
    DrawChip(hdc, x, y, w, h,
             isA ? kAssignBg : kPlayBg,
             isA ? kAssign   : kPlay,
             kText, FmtMsg(m));
}

void DrawScene(HDC hdc, const RECT& rc)
{
    EnsureFonts();

    HDC     mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HBITMAP old = static_cast<HBITMAP>(SelectObject(mem, bmp));

    FillRectColor(mem, rc, kBg);
    SetBkMode(mem, TRANSPARENT);

    Snapshot s = ReadSnapshot();

    // ---- title bar ----
    {
        SelectObject(mem, g_font_title);
        SetTextColor(mem, kText);
        const char* title = "uniflow message_dispatch - professor + friend -> student";
        TextOutA(mem, 24, 18, title, lstrlenA(title));
        SelectObject(mem, g_font_small);
        SetTextColor(mem, kMuted);
        const char* sub = "one Runtime, three modules, one mailbox - all lock-free";
        TextOutA(mem, 24, 44, sub, lstrlenA(sub));
    }

    // ---- mailbox panel ----
    RECT mailbox{24, 72, rc.right - 24, 178};
    {
        std::ostringstream os;
        os << "Mailbox  (" << s.queue.size() << " queued)";
        std::string title = os.str();
        DrawPanel(mem, mailbox, title.c_str());

        const int chip_h = 26;
        const int chip_w = 230;
        const int margin_x = mailbox.left + 14;
        const int margin_y = mailbox.top + 36;
        const int gap_x = 8;
        const int gap_y = 6;
        const int avail_w = mailbox.right - mailbox.left - 28;
        const int cols = (std::max)(1, (avail_w + gap_x) / (chip_w + gap_x));

        for (std::size_t i = 0; i < s.queue.size(); ++i)
        {
            int row = static_cast<int>(i) / cols;
            int col = static_cast<int>(i) % cols;
            int x = margin_x + col * (chip_w + gap_x);
            int y = margin_y + row * (chip_h + gap_y);
            if (y + chip_h > mailbox.bottom - 8) break;
            DrawChipForMessage(mem, x, y, chip_w, chip_h, s.queue[i]);
        }
        if (s.queue.empty())
        {
            SelectObject(mem, g_font_body);
            SetTextColor(mem, kMuted);
            const char* empty = "(empty - student is resting or spawners are between bursts)";
            TextOutA(mem, mailbox.left + 14, mailbox.top + 42,
                     empty, lstrlenA(empty));
        }
    }

    // ---- professor + friend panels (side by side) ----
    int mid = (rc.right) / 2;
    RECT prof_r{24, 192, mid - 6, 380};
    RECT frnd_r{mid + 6, 192, rc.right - 24, 380};
    {
        std::ostringstream os;
        os << "Professor  (" << s.prof_emitted << " / " << s.prof_total
           << " sent" << (s.prof_idle && s.prof_emitted >= s.prof_total
                          ? "  -  done" : "") << ")";
        DrawPanel(mem, prof_r, os.str().c_str());

        SelectObject(mem, g_font_small);
        SetTextColor(mem, kMuted);
        std::string phase = std::string("phase: ") + s.prof_phase;
        TextOutA(mem, prof_r.left + 14, prof_r.top + 30,
                 phase.c_str(), static_cast<int>(phase.size()));

        SelectObject(mem, g_font_small);
        SetTextColor(mem, kMuted);
        const char* pending = "pending:";
        TextOutA(mem, prof_r.left + 14, prof_r.top + 52,
                 pending, lstrlenA(pending));

        int y = prof_r.top + 72;
        for (const auto& m : s.prof_pending)
        {
            if (y + 26 > prof_r.bottom - 8) break;
            DrawChipForMessage(mem, prof_r.left + 14, y,
                               prof_r.right - prof_r.left - 28, 26, m);
            y += 30;
        }
        if (s.prof_pending.empty())
        {
            SelectObject(mem, g_font_body);
            SetTextColor(mem, kMuted);
            TextOutA(mem, prof_r.left + 14, y, "(none)", 6);
        }
    }
    {
        std::ostringstream os;
        os << "Friend  (" << s.friend_emitted << " / " << s.friend_total
           << " sent" << (s.friend_idle && s.friend_emitted >= s.friend_total
                          ? "  -  done" : "") << ")";
        DrawPanel(mem, frnd_r, os.str().c_str());

        SelectObject(mem, g_font_small);
        SetTextColor(mem, kMuted);
        std::string phase = std::string("phase: ") + s.friend_phase;
        TextOutA(mem, frnd_r.left + 14, frnd_r.top + 30,
                 phase.c_str(), static_cast<int>(phase.size()));

        SelectObject(mem, g_font_small);
        SetTextColor(mem, kMuted);
        const char* pending = "pending:";
        TextOutA(mem, frnd_r.left + 14, frnd_r.top + 52,
                 pending, lstrlenA(pending));

        int y = frnd_r.top + 72;
        for (const auto& m : s.friend_pending)
        {
            if (y + 26 > frnd_r.bottom - 8) break;
            DrawChipForMessage(mem, frnd_r.left + 14, y,
                               frnd_r.right - frnd_r.left - 28, 26, m);
            y += 30;
        }
        if (s.friend_pending.empty())
        {
            SelectObject(mem, g_font_body);
            SetTextColor(mem, kMuted);
            TextOutA(mem, frnd_r.left + 14, y, "(none)", 6);
        }
    }

    // ---- student panel ----
    RECT stu_r{24, 394, rc.right - 24, rc.bottom - 24};
    {
        DrawPanel(mem, stu_r, "Student");

        // status dot + current message
        int dot_x = stu_r.left + 14;
        int dot_y = stu_r.top  + 38;
        COLORREF dot = s.student_idle ? kMuted : kAccent;
        RECT     dr{dot_x, dot_y, dot_x + 12, dot_y + 12};
        FillRectColor(mem, dr, dot);

        SelectObject(mem, g_font_body);
        SetTextColor(mem, kText);
        std::string status = s.student_idle
                             ? "idle"
                             : std::string("active: ") + FmtMsg(s.student_current);
        TextOutA(mem, dot_x + 24, dot_y - 2,
                 status.c_str(), static_cast<int>(status.size()));

        SelectObject(mem, g_font_small);
        SetTextColor(mem, kMuted);
        std::string phase = std::string("phase: ") + s.student_phase;
        TextOutA(mem, stu_r.left + 14, stu_r.top + 64,
                 phase.c_str(), static_cast<int>(phase.size()));

        // bars
        int bar_w = (stu_r.right - stu_r.left - 70) / 2;
        int bar_h = 18;
        int bar_y = stu_r.top + 116;
        DrawBar(mem, stu_r.left + 24, bar_y, bar_w, bar_h,
                s.student_ability, 10, kAbility, "ability");

        COLORREF stress_fill =
            s.student_stress >= GlobalConfig::kStressMax - 1 ? kStressHi
            : s.student_stress >= GlobalConfig::kStressMax / 2 ? RGB(210, 160, 80)
            : kStressLo;
        DrawBar(mem, stu_r.left + 44 + bar_w, bar_y, bar_w, bar_h,
                s.student_stress, GlobalConfig::kStressMax,
                stress_fill, "stress");

        // counters
        SelectObject(mem, g_font_body);
        SetTextColor(mem, kText);
        std::ostringstream os;
        os << "hours spent: " << s.student_hours
           << "        messages handled: " << s.student_done;
        std::string line = os.str();
        TextOutA(mem, stu_r.left + 24, stu_r.top + 158,
                 line.c_str(), static_cast<int>(line.size()));
    }

    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
        SetTimer(hwnd, 1, 33, nullptr);  // ~30 fps
        return 0;
    case WM_TIMER:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC         hdc = BeginPaint(hwnd, &ps);
        RECT        rc;
        GetClientRect(hwnd, &rc);
        DrawScene(hdc, rc);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

} // anonymous

void RunVisualisation()
{
    const char* cls = "uniflow_message_dispatch";
    WNDCLASSA   wc{};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandle(nullptr);
    wc.lpszClassName = cls;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowA(cls, "message_dispatch",
                              WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1100, 720,
                              nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_font_title) { DeleteObject(g_font_title); g_font_title = nullptr; }
    if (g_font_h)     { DeleteObject(g_font_h);     g_font_h = nullptr; }
    if (g_font_body)  { DeleteObject(g_font_body);  g_font_body = nullptr; }
    if (g_font_small) { DeleteObject(g_font_small); g_font_small = nullptr; }
}

#else // ----- non-Windows or UNIFLOW_CONSOLE_VIZ: tiny console summary -----

void RunVisualisation()
{
    const auto t0 = std::chrono::steady_clock::now();
    while (!GlobalEnv::Stop()
           && std::chrono::steady_clock::now() - t0 < 60s)
    {
        Snapshot s = ReadSnapshot();
        std::cout << "[viz] queue=" << s.queue.size()
                  << "  prof=" << s.prof_emitted << "/" << s.prof_total
                  << "  friend=" << s.friend_emitted << "/" << s.friend_total
                  << "  ability=" << s.student_ability
                  << "  stress=" << s.student_stress
                  << "  done=" << s.student_done << "\n";
        std::this_thread::sleep_for(500ms);
    }
}

#endif
