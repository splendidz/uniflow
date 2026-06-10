#include "uf_visualization.h"

#include "app.h"
#include "mailbox.h"
#include "snapshot.h"
#include "uf_receiver.h"
#include "uf_sender.h"

#include <chrono>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

using namespace std::chrono_literals;

UF_Visualization::StepResult UF_Visualization::OnViz_Begin()
{
    return UF_NEXT(OnViz_Tick);
}

UF_Visualization::StepResult UF_Visualization::OnViz_Tick()
{
    if (GlobalEnv::Stop()) return Done();
    const auto& s = App::inst().send;
    const auto& r = App::inst().recv;
    {
        std::lock_guard<std::mutex> lk(g_snap_mu);
        g_snap.vec_a            = s.VecA();
        g_snap.vec_b            = s.VecB();
        g_snap.queue            = Mailbox::Snapshot();
        g_snap.last_burst_count = s.LastBurstCount();
        g_snap.total_bursts     = s.TotalBursts();
        g_snap.recv_state       = r.State();
        g_snap.processed        = r.Processed();
        g_snap.last_result      = r.LastResult();
        g_snap.current          = r.Current();
        g_snap.sender_phase     = s.CurrentStepDescription();
        g_snap.recv_phase       = r.CurrentStepDescription();
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

static std::string FormatMsg(const Msg& m)
{
    std::ostringstream os;
    os << m.a << ' ' << m.op << ' ' << m.b;
    return os.str();
}

static void DrawScene(HDC hdc, const RECT& rc)
{
    HDC     mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HBITMAP old = static_cast<HBITMAP>(SelectObject(mem, bmp));

    HBRUSH bg = CreateSolidBrush(RGB(20, 24, 30));
    FillRect(mem, &rc, bg);
    DeleteObject(bg);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(220, 220, 230));

    Snapshot s = ReadSnapshot();

    const char* hdr = "uniflow queue_drain - sender bursts, receiver drains";
    TextOutA(mem, 20, 12, hdr, lstrlenA(hdr));

    // left column: source vectors
    int y = 50;
    TextOutA(mem, 20, y, "vec A:", 6);
    TextOutA(mem, 20, y + 20, "vec B:", 6);
    int x = 80;
    for (std::size_t i = 0; i < s.vec_a.size(); ++i)
    {
        std::string sa = std::to_string(s.vec_a[i]);
        std::string sb = std::to_string(s.vec_b[i]);
        TextOutA(mem, x, y,      sa.c_str(), static_cast<int>(sa.size()));
        TextOutA(mem, x, y + 20, sb.c_str(), static_cast<int>(sb.size()));
        x += 28;
    }

    // sender stats
    {
        std::ostringstream os;
        os << "sender: bursts=" << s.total_bursts
           << "  last_burst=" << s.last_burst_count
           << "  phase=" << s.sender_phase;
        std::string line = os.str();
        TextOutA(mem, 20, 100, line.c_str(), static_cast<int>(line.size()));
    }

    // queue display
    TextOutA(mem, 20, 140, "queue:", 6);
    {
        HBRUSH qb = CreateSolidBrush(RGB(40, 60, 90));
        int    qx = 90;
        int    qy = 132;
        int    cell_w = 80;
        int    cell_h = 28;
        for (std::size_t i = 0; i < s.queue.size() && i < 20; ++i)
        {
            RECT cell{qx, qy, qx + cell_w - 4, qy + cell_h};
            FillRect(mem, &cell, qb);
            std::string body = FormatMsg(s.queue[i]);
            TextOutA(mem, qx + 6, qy + 6, body.c_str(),
                     static_cast<int>(body.size()));
            qx += cell_w;
            if (qx + cell_w > rc.right - 20)
            {
                qx = 90;
                qy += cell_h + 4;
            }
        }
        DeleteObject(qb);
    }

    // receiver state
    {
        const char* state_str = ToString(s.recv_state);
        COLORREF c =
            s.recv_state == RecvState::Adding      ? RGB(90, 170, 110) :
            s.recv_state == RecvState::Subtracting ? RGB(200, 120, 60) :
            s.recv_state == RecvState::Dispatching ? RGB(170, 150, 60) :
                                                     RGB(90, 94, 104);
        HBRUSH rb = CreateSolidBrush(c);
        RECT   box{20, 260, 180, 300};
        FillRect(mem, &box, rb);
        DeleteObject(rb);
        TextOutA(mem, 30, 270, state_str, lstrlenA(state_str));

        std::ostringstream os;
        os << "receiver: processed=" << s.processed
           << "   phase=" << s.recv_phase;
        std::string line = os.str();
        TextOutA(mem, 200, 270, line.c_str(),
                 static_cast<int>(line.size()));

        std::string last = "last: " + s.last_result;
        TextOutA(mem, 200, 290, last.c_str(),
                 static_cast<int>(last.size()));
    }

    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_CREATE:
        SetTimer(hwnd, 1, 16, nullptr);
        return 0;
    case WM_TIMER:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
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

void RunVisualisation()
{
    const char* cls = "uniflow_queue_drain";
    WNDCLASSA   wc{};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandle(nullptr);
    wc.lpszClassName = cls;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowA(cls, "queue_drain",
                              WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1100, 380,
                              nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

#else // ----- non-Windows: console animation -----

void RunVisualisation()
{
    const auto t0 = std::chrono::steady_clock::now();
    std::cout << "queue_drain console viz (sender stops after burst budget).\n";
    while (!GlobalEnv::Stop()
           && std::chrono::steady_clock::now() - t0 < 60s)
    {
        Snapshot s = ReadSnapshot();
        std::cout << "[viz] bursts=" << s.total_bursts
                  << "  queue=" << s.queue.size()
                  << "  recv=" << ToString(s.recv_state)
                  << "  processed=" << s.processed
                  << "  last=\"" << s.last_result << "\"\n";
        std::this_thread::sleep_for(250ms);
    }
}

#endif
