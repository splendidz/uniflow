// viz.cpp - Win32 / console renderer. Define UNIFLOW_CONSOLE_VIZ on
// Windows to force the console backend.
#include "uf_visualization.h"

#include "app.h"
#include "globals.h"
#include "snapshot.h"
#include "uf_load_picker.h"
#include "uf_stage.h"
#include "uf_unload_picker.h"

#include <chrono>
#include <cmath>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

using namespace std::chrono_literals;

UF_Visualization::StepResult UF_Visualization::OnViz_Begin()
{
    return UF_NEXT(OnViz_Tick);
}

UF_Visualization::StepResult UF_Visualization::OnViz_Tick()
{
    if (GlobalEnv::Stop())
        return Done();
    const auto& load   = App::inst().load;
    const auto& unload = App::inst().unload;
    const auto& stage  = App::inst().stage;
    {
        std::lock_guard<std::mutex> lk(g_snap_mu);
        g_snap.load_x_mm            = load.X_mm();
        g_snap.load_z_mm            = load.Z_mm();
        g_snap.load_carry           = load.Carrying();
        g_snap.load_finger_gap_mm   = load.FingerGap_mm();
        g_snap.load_phase           = load.CurrentStepDescription();
        g_snap.unload_x_mm          = unload.X_mm();
        g_snap.unload_z_mm          = unload.Z_mm();
        g_snap.unload_carry         = unload.Carrying();
        g_snap.unload_finger_gap_mm = unload.FingerGap_mm();
        g_snap.unload_phase         = unload.CurrentStepDescription();
        g_snap.stage_table_x_mm     = stage.TableX_mm();
        g_snap.stage_table_y_mm     = stage.TableY_mm();
        g_snap.stage_state          = stage.state();
        g_snap.stage_phase          = stage.CurrentStepDescription();
        g_snap.zoneA_has_part       = GlobalEnv::ZoneAHasPart();
        g_snap.delivered            = GlobalEnv::DeliveredCount();
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

static int SX(double x) { return 60 + static_cast<int>(x / GlobalGeometry::kXMax_mm * 860.0); }
static int SZ(double z) { return 96 + static_cast<int>(z / GlobalGeometry::kZDown_mm * 210.0); }

static constexpr double kFingerPxPerMm = 1.5;  // gripper-only px/mm
static int FingerPx(double mm) { return static_cast<int>(mm * kFingerPxPerMm); }

static void DrawScene(HDC hdc, const RECT& rc)
{
    HDC     mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HBITMAP old = static_cast<HBITMAP>(SelectObject(mem, bmp));

    HBRUSH bg = CreateSolidBrush(RGB(24, 26, 32));
    FillRect(mem, &rc, bg);
    DeleteObject(bg);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(210, 214, 222));

    Snapshot s = ReadSnapshot();

    HPEN rail = CreatePen(PS_SOLID, 3, RGB(70, 74, 84));
    HPEN oldp = static_cast<HPEN>(SelectObject(mem, rail));
    MoveToEx(mem, 50, 70, nullptr); LineTo(mem, 930, 70);
    MoveToEx(mem, 50, 86, nullptr); LineTo(mem, 930, 86);
    SelectObject(mem, oldp);
    DeleteObject(rail);

    auto zone = [&](double zx, const char* label, COLORREF c)
    {
        RECT   z{SX(zx) - 56, 330, SX(zx) + 56, 372};
        HBRUSH b = CreateSolidBrush(c);
        FillRect(mem, &z, b);
        DeleteObject(b);
        TextOutA(mem, SX(zx) - 50, 376, label, lstrlenA(label));
    };
    zone(GlobalGeometry::kZoneA_mm, "A  Load",   RGB(40, 60, 90));
    zone(GlobalGeometry::kZoneB_mm, "B  Stage",  RGB(70, 55, 90));
    zone(GlobalGeometry::kZoneC_mm, "C  Unload", RGB(40, 80, 60));

    HPEN span = CreatePen(PS_DOT, 1, RGB(120, 90, 130));
    oldp      = static_cast<HPEN>(SelectObject(mem, span));
    MoveToEx(mem, SX(GlobalGeometry::kZoneB_mm - GlobalGeometry::kBSafetyGap_mm), 60, nullptr);
    LineTo  (mem, SX(GlobalGeometry::kZoneB_mm - GlobalGeometry::kBSafetyGap_mm), 330);
    MoveToEx(mem, SX(GlobalGeometry::kZoneB_mm + GlobalGeometry::kBSafetyGap_mm), 60, nullptr);
    LineTo  (mem, SX(GlobalGeometry::kZoneB_mm + GlobalGeometry::kBSafetyGap_mm), 330);
    SelectObject(mem, oldp);
    DeleteObject(span);

    {
        int    sx = SX(s.stage_table_x_mm);
        int    sy = 318 + static_cast<int>(s.stage_table_y_mm / 40.0 * 8.0);
        RECT   t{sx - 34, sy - 12, sx + 34, sy + 12};
        COLORREF c =
            s.stage_state == StageState::Processing         ? RGB(200, 120, 60) :
            s.stage_state == StageState::ProcessedPartReady ? RGB(90, 170, 110) :
            s.stage_state == StageState::RawPartLoaded      ? RGB(180, 160, 60) :
                                                              RGB(90, 94, 104);
        HBRUSH b = CreateSolidBrush(c);
        FillRect(mem, &t, b);
        DeleteObject(b);
    }

    if (s.zoneA_has_part)
    {
        RECT   p{SX(GlobalGeometry::kZoneA_mm) - 11, 308,
                 SX(GlobalGeometry::kZoneA_mm) + 11, 330};
        HBRUSH b = CreateSolidBrush(RGB(210, 180, 70));
        FillRect(mem, &p, b);
        DeleteObject(b);
    }

    // picker = arm + base block + two fingers (gap_mm), part drawn
    // between fingers when carrying.
    auto picker = [&](double px, double pz, bool carry, double gap_mm,
                      int rail_y, COLORREF arm_color, const char* tag,
                      const std::string& phase)
    {
        const int hx   = SX(px);
        const int hy   = SZ(pz);
        const int base_y      = hy;                  // top of fingers
        const int finger_len  = 20;
        const int finger_tip  = base_y + finger_len; // bottom of fingers
        const int half_gap_px = FingerPx(gap_mm) / 2;
        const int finger_w    = 4;                   // pixel width of each finger
        const int part_half_w = FingerPx(GlobalGeometry::kPartWidth_mm) / 2;

        // arm
        HPEN arm = CreatePen(PS_SOLID, 5, arm_color);
        oldp     = static_cast<HPEN>(SelectObject(mem, arm));
        MoveToEx(mem, hx, rail_y, nullptr);
        LineTo  (mem, hx, base_y);
        SelectObject(mem, oldp);
        DeleteObject(arm);

        // gripper base block (small horizontal plate the fingers hang off)
        {
            HBRUSH bb = CreateSolidBrush(arm_color);
            RECT   b{hx - 14, base_y - 5, hx + 14, base_y + 3};
            FillRect(mem, &b, bb);
            DeleteObject(bb);
        }

        // two fingers
        {
            HBRUSH fb = CreateSolidBrush(arm_color);
            RECT   left { hx - half_gap_px - finger_w, base_y,
                          hx - half_gap_px,            finger_tip };
            RECT   right{ hx + half_gap_px,            base_y,
                          hx + half_gap_px + finger_w, finger_tip };
            FillRect(mem, &left,  fb);
            FillRect(mem, &right, fb);
            DeleteObject(fb);
        }

        // part being carried, drawn between (or behind) the fingers
        if (carry)
        {
            HBRUSH pb = CreateSolidBrush(RGB(210, 180, 70));
            RECT   p{ hx - part_half_w, base_y + 2,
                      hx + part_half_w, finger_tip - 2 };
            FillRect(mem, &p, pb);
            DeleteObject(pb);
        }

        std::string label = std::string(tag) + " " + phase;
        TextOutA(mem, hx - 34, rail_y - 18, label.c_str(),
                 static_cast<int>(label.size()));
    };
    picker(s.load_x_mm,   s.load_z_mm,   s.load_carry,   s.load_finger_gap_mm,
           70, RGB(90, 150, 230),  "LD", s.load_phase);
    picker(s.unload_x_mm, s.unload_z_mm, s.unload_carry, s.unload_finger_gap_mm,
           86, RGB(230, 130, 90), "UL", s.unload_phase);

    const char* hdr = "uniflow CNC line - LoadPicker / Stage / UnloadPicker, "
                      "orchestrator-driven, lock-free B-zone hand-off";
    TextOutA(mem, 50, 16, hdr, lstrlenA(hdr));
    std::string done = "delivered at C: " + std::to_string(s.delivered);
    TextOutA(mem, 760, 16, done.c_str(), static_cast<int>(done.size()));
    std::string stg = std::string("stage: ") + ToString(s.stage_state)
                      + " (" + s.stage_phase + ")";
    TextOutA(mem, 50, 36, stg.c_str(), static_cast<int>(stg.size()));

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
    const char* cls = "uniflow_cnc";
    WNDCLASSA   wc{};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandle(nullptr);
    wc.lpszClassName = cls;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowA(cls, "cnc_pickers",
                              WS_OVERLAPPEDWINDOW & ~WS_THICKFRAME,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1000, 470,
                              nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

#else // ----- non-Windows (or UNIFLOW_CONSOLE_VIZ): console animation -----

void RunVisualisation()
{
    auto cell = [](double x)
    {
        return static_cast<int>(x / GlobalGeometry::kXMax_mm * 64.0);
    };
    const auto t0 = std::chrono::steady_clock::now();
    std::cout << "uniflow CNC line (console). Runs ~20s.\n";
    while (std::chrono::steady_clock::now() - t0 < 20s)
    {
        Snapshot s = ReadSnapshot();
        std::string track(66, ' ');
        track[cell(GlobalGeometry::kZoneA_mm)] = 'A';
        track[cell(GlobalGeometry::kZoneB_mm)] = 'B';
        track[cell(GlobalGeometry::kZoneC_mm)] = 'C';
        std::string ld(66, ' '), ul(66, ' ');
        ld[cell(s.load_x_mm)]   = s.load_carry   ? '#' : 'v';
        ul[cell(s.unload_x_mm)] = s.unload_carry ? '#' : 'v';
        std::cout << "\r LD[" << ld << "] " << s.load_phase << "        \n"
                  << " UL[" << ul << "] " << s.unload_phase << "        \n"
                  << "   [" << track << "]  stage=" << ToString(s.stage_state)
                  << " delivered=" << s.delivered << "   \x1b[3A" << std::flush;
        std::this_thread::sleep_for(80ms);
    }
    std::cout << "\x1b[3B\n";
}

#endif
