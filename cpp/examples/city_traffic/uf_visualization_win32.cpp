// uf_visualization_win32.cpp - Win32 GDI render back-end (Windows only).
// Renders the static map, live per-approach signals, and the vehicles into a
// window. On non-Windows platforms this compiles to a no-op stub; the console
// back-end (uf_visualization_console.cpp) is used there instead.
#include "uf_visualization.h"

#if defined(_WIN32)

#include "globals.h"
#include "map.h"
#include "snapshot.h"
#include "world.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

#include <cmath>

using namespace citymap;

// Grid->pixel transform. Node POSITIONS use sx/sy independently so the (square)
// map stretches to fill the window width. LOCAL sizes (road width, cars, lane
// offsets, arrows) use the single scale 's' so nothing looks squashed - only
// the spacing between intersections is wider.
struct Tx
{
    double sx; // pixels per grid unit, x (positions)
    double sy; // pixels per grid unit, y (positions)
    double s;  // uniform scale for sizes (= min(sx, sy))
    int    ox;
    int    oy;
};

// Margins leave room for the road/junction-pad half-width that spills past the
// boundary nodes (otherwise the edge roads get clipped).
static int Margin() { return 60; }     // sides and bottom
static int TopMargin() { return 104; } // header band above the map

static Tx MakeTx(const RECT& rc)
{
    double gw     = GridMaxX() - GridMinX();
    double gh     = GridMaxY() - GridMinY();
    double availW = rc.right - 2 * Margin();
    double availH = rc.bottom - TopMargin() - Margin();
    // Uniform scale -> a square map, so the stopping gap looks identical on
    // horizontal and vertical roads. Centred in the area below the header.
    double s  = (availW / gw < availH / gh) ? availW / gw : availH / gh;
    int    cw = static_cast<int>(s * gw);
    int    ch = static_cast<int>(s * gh);
    int    ox = (rc.right - cw) / 2;
    int    oy = TopMargin() + (static_cast<int>(availH) - ch) / 2;
    return Tx{s, s, s, ox, oy};
}

static int PX(double gx, const Tx& t)
{
    return t.ox + static_cast<int>((gx - GridMinX()) * t.sx);
}
static int PY(double gy, const Tx& t)
{
    return t.oy + static_cast<int>((gy - GridMinY()) * t.sy);
}

static void DrawScene(HDC hdc, const RECT& rc)
{
    HDC     mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HBITMAP old = static_cast<HBITMAP>(SelectObject(mem, bmp));

    HBRUSH bg = CreateSolidBrush(RGB(30, 32, 38));
    FillRect(mem, &rc, bg);
    DeleteObject(bg);
    SetBkMode(mem, TRANSPARENT);

    const Tx t = MakeTx(rc);

    // road surface (width scales with the view so two car widths fit)
    int  roadpx = static_cast<int>(geo::kRoadWidth * t.s);
    HPEN road   = CreatePen(PS_SOLID, roadpx, RGB(58, 62, 70));
    HPEN oldp   = static_cast<HPEN>(SelectObject(mem, road));
    for (const Edge& e : Edges())
    {
        const Node& a = NodeById(e.a);
        const Node& b = NodeById(e.b);
        MoveToEx(mem, PX(a.gx, t), PY(a.gy, t), nullptr);
        LineTo  (mem, PX(b.gx, t), PY(b.gy, t));
    }
    SelectObject(mem, oldp);
    DeleteObject(road);

    // dashed yellow centre line
    HPEN lane = CreatePen(PS_DOT, 1, RGB(205, 185, 75));
    oldp      = static_cast<HPEN>(SelectObject(mem, lane));
    for (const Edge& e : Edges())
    {
        const Node& a = NodeById(e.a);
        const Node& b = NodeById(e.b);
        MoveToEx(mem, PX(a.gx, t), PY(a.gy, t), nullptr);
        LineTo  (mem, PX(b.gx, t), PY(b.gy, t));
    }
    SelectObject(mem, oldp);
    DeleteObject(lane);

    Snapshot s = ReadSnapshot();

    // junction pads (sized to the road; no labels)
    int bh = roadpx / 2 + 6;
    for (const Node& n : Nodes())
    {
        if (n.kind == NodeKind::Corner)
            continue;
        int      x = PX(n.gx, t);
        int      y = PY(n.gy, t);
        COLORREF c = n.kind == NodeKind::FourWay ? RGB(72, 96, 138)
                                                 : RGB(82, 116, 92);
        HBRUSH   b = CreateSolidBrush(c);
        RECT     box{x - bh, y - bh, x + bh, y + bh};
        FillRect(mem, &box, b);
        DeleteObject(b);
    }

    // vehicles: a small car (body + four wheels + headlights), oriented to
    // its heading and placed on the right-hand lane. Parts are laid out in the
    // car's local frame: +u along travel, +v to the driver's right.
    for (const VehicleView& v : s.vehicles)
    {
        double dx = v.dx, dy = v.dy;
        double L  = std::sqrt(dx * dx + dy * dy);
        if (L < 1e-6) { dx = 1.0; dy = 0.0; L = 1.0; }
        dx /= L; dy /= L;
        double rx = -dy, ry = dx; // right of travel (screen y-down)

        int cx = PX(v.gx, t) + static_cast<int>(rx * geo::kLaneHalf * t.s);
        int cy = PY(v.gy, t) + static_cast<int>(ry * geo::kLaneHalf * t.s);

        // local (u along forward, vv along right) in grid units -> pixel point
        auto pt = [&](double u, double vv) -> POINT {
            return POINT{cx + static_cast<LONG>((u * dx + vv * rx) * t.s),
                         cy + static_cast<LONG>((u * dy + vv * ry) * t.s)};
        };
        HPEN op = static_cast<HPEN>(
            SelectObject(mem, CreatePen(PS_SOLID, 1, RGB(15, 16, 20))));

        auto orect = [&](double cu, double cv, double hl, double hw, COLORREF c) {
            POINT q[4] = {pt(cu - hl, cv - hw), pt(cu + hl, cv - hw),
                          pt(cu + hl, cv + hw), pt(cu - hl, cv + hw)};
            HBRUSH br = CreateSolidBrush(c);
            HBRUSH ob = static_cast<HBRUSH>(SelectObject(mem, br));
            Polygon(mem, q, 4);
            SelectObject(mem, ob);
            DeleteObject(br);
        };

        const double   Lh    = 0.048; // body half-length (grid)
        const double   Wh    = 0.022; // body half-width
        const COLORREF wheel = RGB(170, 172, 178); // grey, visible on dark road

        // four wheels, peeking out past the body sides
        orect( 0.032,  Wh, 0.017, 0.010, wheel);
        orect( 0.032, -Wh, 0.017, 0.010, wheel);
        orect(-0.032,  Wh, 0.017, 0.010, wheel);
        orect(-0.032, -Wh, 0.017, 0.010, wheel);
        // body
        orect(0.0, 0.0, Lh, Wh, RGB(v.r, v.g, v.b));

        // two headlights at the front corners (these show which way it faces)
        auto dot = [&](double u, double vv, int rad, COLORREF c) {
            POINT  p  = pt(u, vv);
            HBRUSH br = CreateSolidBrush(c);
            HBRUSH ob = static_cast<HBRUSH>(SelectObject(mem, br));
            Ellipse(mem, p.x - rad, p.y - rad, p.x + rad, p.y + rad);
            SelectObject(mem, ob);
            DeleteObject(br);
        };
        dot(Lh - 0.003,  Wh * 0.62, 3, RGB(255, 244, 205));
        dot(Lh - 0.003, -Wh * 0.62, 3, RGB(255, 244, 205));

        // turn signal: one larger neon-amber lamp over the headlight on the
        // side the car is turning to (+v = right).
        if (v.blink != 0 && (GetTickCount() % 600) < 330)
        {
            double cv = (v.blink > 0 ? Wh : -Wh) * 0.62;
            dot(Lh - 0.003, cv, 5, RGB(255, 205, 35));
        }

        DeleteObject(SelectObject(mem, op)); // restore + delete our pen
    }

    // live signals: arrows inside the junction. Each approach shows a
    // straight/right arrow and a left-turn arrow, lit red/amber/green.
    auto sigColor = [](city::LightColor c) -> COLORREF {
        switch (c)
        {
        case city::LightColor::Green:  return RGB(50, 215, 95);
        case city::LightColor::Yellow: return RGB(240, 200, 50);
        default:                       return RGB(120, 40, 40); // dim red = stop
        }
    };
    // Line arrow drawn in pixels (so it stays undistorted under the stretch).
    // (cx,cy) is the pixel centre; 'dx,dy' the incoming axis direction; 'sg'
    // the half-size in grid units. 'bend': 0 straight, -1 left, +1 right -
    // a bent arrow turns 90 degrees that way so it reads as a turn arrow.
    auto drawArrow = [&](double cx, double cy, double dx, double dy, double sg,
                         int bend, COLORREF c) {
        double L = std::sqrt(dx * dx + dy * dy);
        if (L < 1e-9)
            return;
        dx /= L; dy /= L;
        double s   = sg * t.s; // pixels
        int    pw  = std::max(2, static_cast<int>(0.012 * t.s));
        HPEN   pen = CreatePen(PS_SOLID, pw, c);
        HPEN   op  = static_cast<HPEN>(SelectObject(mem, pen));
        auto   seg = [&](double x0, double y0, double x1, double y1) {
            MoveToEx(mem, static_cast<int>(x0), static_cast<int>(y0), nullptr);
            LineTo(mem, static_cast<int>(x1), static_cast<int>(y1));
        };
        double ex, ey; // exit direction
        if (bend == 0)      { ex = dx;  ey = dy;  }
        else if (bend < 0)  { ex = dy;  ey = -dx; } // left
        else                { ex = -dy; ey = dx;  } // right
        double tipx, tipy;
        if (bend != 0)
        {
            seg(cx - dx * s, cy - dy * s, cx, cy); // incoming leg
            tipx = cx + ex * s;
            tipy = cy + ey * s;
            seg(cx, cy, tipx, tipy);               // turn leg
        }
        else
        {
            seg(cx - dx * s, cy - dy * s, cx + dx * s, cy + dy * s);
            tipx = cx + dx * s;
            tipy = cy + dy * s;
        }
        // arrowhead at the tip, pointing along (ex,ey)
        double hl = s * 0.6, hw = s * 0.5;
        double perx = -ey, pery = ex;
        seg(tipx, tipy, tipx - ex * hl + perx * hw, tipy - ey * hl + pery * hw);
        seg(tipx, tipy, tipx - ex * hl - perx * hw, tipy - ey * hl - pery * hw);
        SelectObject(mem, op);
        DeleteObject(pen);
    };

    for (const Node& n : Nodes())
    {
        if (n.kind == NodeKind::Corner)
            continue;
        if (n.id >= static_cast<int>(s.signals.size()))
            continue;
        const city::SignalState& st = s.signals[n.id];

        for (int nb : NeighborsOf(n.id))
        {
            const Node& m  = NodeById(nb);
            double      ux = m.gx - n.gx; // outward from the junction
            double      uy = m.gy - n.gy;
            double      L  = std::sqrt(ux * ux + uy * uy);
            if (L < 1e-9)
                continue;
            ux /= L; uy /= L;
            double rxp = uy, ryp = -ux;   // right of the incoming traveller

            city::Axis ax  = city::AxisOfEdge(n, m);
            double     tdx = -ux, tdy = -uy; // travel-through direction

            // Only show arrows for movements that actually have an exit here,
            // so a three-way junction has no phantom (e.g. left-turn) signal.
            bool hasS = false, hasL = false, hasR = false;
            for (int e : NeighborsOf(n.id))
            {
                if (e == nb)
                    continue; // the road we came in on
                const Node& en  = NodeById(e);
                double      ohx = en.gx - n.gx, ohy = en.gy - n.gy;
                double      Lo  = std::sqrt(ohx * ohx + ohy * ohy);
                if (Lo < 1e-9)
                    continue;
                ohx /= Lo; ohy /= Lo;
                double dot = tdx * ohx + tdy * ohy;
                if (dot > 0.8)
                    hasS = true;
                else if (tdx * ohy - tdy * ohx > 0)
                    hasR = true;
                else
                    hasL = true;
            }

            int      nodeX = PX(n.gx, t);
            int      nodeY = PY(n.gy, t);
            double   inOff = 0.085;
            // No left turns in this build: draw straight and right only, lit by
            // this approach's single green.
            COLORREF c = sigColor(city::StraightLight(st, ax));
            auto place = [&](double lat, int bend) {
                double ofx = ux * inOff + rxp * lat;
                double ofy = uy * inOff + ryp * lat;
                drawArrow(nodeX + ofx * t.s, nodeY + ofy * t.s, tdx, tdy, 0.05,
                          bend, c);
            };
            if (hasS) place(geo::kLaneHalf,        0);  // straight
            if (hasR) place(geo::kLaneHalf + 0.05, +1); // right
            (void)hasL; // left turns disabled
        }
    }

    SetTextColor(mem, RGB(235, 237, 242));
    const char* hdr = "uniflow city traffic - a fleet on one pump thread: "
                      "signals, acceleration, car-ahead safety distance, "
                      "intersection yielding";
    TextOutA(mem, 24, 16, hdr, lstrlenA(hdr));

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
    return DefWindowProcA(hwnd, msg, wp, lp);
}

void RunVisualisationWin32()
{
    const char* cls = "uniflow_city_traffic";
    WNDCLASSA   wc{};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandle(nullptr);
    wc.lpszClassName = cls;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowA(cls, "city_traffic",
                              WS_OVERLAPPEDWINDOW,
                              CW_USEDEFAULT, CW_USEDEFAULT, 1120, 880,
                              nullptr, nullptr, wc.hInstance, nullptr);
    ShowWindow(hwnd, SW_SHOW);

    MSG msg;
    while (GetMessageA(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
}

#else  // !_WIN32

// Off Windows there is no Win32 window; UseConsoleRenderer() is always true so
// this is never called. The stub keeps the symbol defined for the linker.
void RunVisualisationWin32() {}

#endif
