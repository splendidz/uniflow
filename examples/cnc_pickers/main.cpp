// ============================================================================
//  cnc_pickers — demo_concept.md problem 2, solved with uniflow
//
//  A CNC-style line: raw parts appear at zone A (Load) at random times. Picker
//  "AB" carries a part A -> B; the Stage machines it at B; picker "BC" carries
//  it B -> C (Unload). Each picker has an x and a z axis. The two pickers must
//  never share the B zone — one must clear B (by >= the safety gap) before the
//  other enters.
//
//  Why uniflow fits:
//   - The two pickers are the SAME class, two instances ("AB", "BC") — exactly
//     what UF_USES_UNIFLOW + named instances is for.
//   - Each picker's pick/carry/place motion is a chain of steps. Motion is
//     paced with Sleep() — smooth, and the pump never busy-spins.
//   - Collision avoidance is a cross-module read: a picker checks the OTHER
//     picker's x via Picker::GetInst(...). That read is safe with no lock
//     because every module shares the one pump thread (DESIGN.md §5-10).
//   - The Stage and the Loader are single-instance modules (UF_SINGLETON).
//
//  Visualisation: a Win32 window on Windows; a console animation elsewhere.
//
//  Build (from the repo root):
//    cl /std:c++17 /EHsc /utf-8 /I include examples\cnc_pickers\main.cpp ^
//       /Fe:build\cnc_pickers.exe /link user32.lib gdi32.lib
//    g++ -std=c++17 -O2 -pthread -I include ^
//       examples/cnc_pickers/main.cpp -o build/cnc_pickers
// ============================================================================
#include "uniflow.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <mutex>
#include <random>
#include <string>
#include <thread>

// Visualisation backend: Win32 window on Windows, console animation elsewhere.
// Define UNIFLOW_CONSOLE_VIZ to force the console backend on Windows too.
#if defined(_WIN32) && !defined(UNIFLOW_CONSOLE_VIZ)
#define UNIFLOW_WIN32_VIZ 1
#else
#define UNIFLOW_WIN32_VIZ 0
#endif

#if UNIFLOW_WIN32_VIZ
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

using namespace std::chrono_literals;
using uniflow::Duration;

// ── Line geometry, in millimetres ───────────────────────────────────────────
static constexpr double kZoneA   = 200.0;  // Load
static constexpr double kZoneB   = 700.0;  // Stage (machining)
static constexpr double kZoneC   = 1200.0; // Unload
static constexpr double kXMax    = 1400.0;
static constexpr double kBClear  = 250.0;  // a picker "occupies B" within this
static constexpr double kZUp     = 0.0;
static constexpr double kZDown   = 120.0;
static constexpr double kXSpeed  = 16.0;   // mm per animation frame
static constexpr double kZSpeed  = 9.0;    // mm per animation frame
static constexpr auto   kFrame   = 16ms;   // animation tick
static constexpr int    kProcessFrames = 70; // Stage machining duration

// True when x is inside the contested B zone.
static bool InB(double x) { return std::fabs(x - kZoneB) < kBClear; }

// ── Shutdown flag — set by the UI thread, read by module steps ──────────────
static std::atomic<bool> g_stop{false};

// ── Sim state shared only between modules (all on the one pump thread, so no
//    locks needed). The Stage owns its own state; these two are the zones. ──
static bool g_zoneA_part = false; // a raw part is waiting at Load
static int  g_delivered  = 0;     // finished parts at Unload

// ════════════════════════════════════════════════════════════════════════════
//  Snapshot — the ONLY cross-thread hand-off. The Viz module fills it on the
//  pump thread; the UI thread reads a copy. One mutex, held only for the copy.
// ════════════════════════════════════════════════════════════════════════════
struct Snapshot
{
    double      ab_x = kZoneA, ab_z = kZUp;
    double      bc_x = kZoneC, bc_z = kZUp;
    bool        ab_carry = false, bc_carry = false;
    std::string ab_phase = "-", bc_phase = "-";
    double      stage_x = kZoneB, stage_y = 0.0;
    int         stage_state = 0; // 0 empty 1 loaded 2 machining 3 done
    bool        zoneA_part = false;
    int         delivered  = 0;
};
static Snapshot   g_snap;
static std::mutex g_snap_mu;

// ════════════════════════════════════════════════════════════════════════════
//  Stage — the machine at zone B. Single instance.
// ════════════════════════════════════════════════════════════════════════════
class Stage : public uniflow::Uniflow<Stage>
{
    UF_SINGLETON(Stage);

public:
    StepResult OnRun_Begin() { return UF_NEXT(OnRun_Idle); }

    // ── Queried by the pickers (same pump thread — no lock) ──
    bool CanReceive() const { return state_ == 0; } // empty
    bool CanPickup() const { return state_ == 3; }  // machined, awaiting BC
    void Receive() { state_ = 1; }                  // AB placed a raw part
    void Pickup() { state_ = 0; }                   // BC took the part

    int    state() const { return state_; }
    double mx() const { return mx_; }
    double my() const { return my_; }

private:
    StepResult OnRun_Idle()
    {
        if (g_stop)
            return Done();
        if (state_ == 1) // a raw part was placed — start machining
        {
            state_   = 2;
            frame_   = 0;
            return UF_NEXT(OnRun_Machine);
        }
        return Sleep(kFrame); // nothing to do — re-check, paced
    }

    StepResult OnRun_Machine()
    {
        if (g_stop)
            return Done();
        // Animate the x/y table tracing a little machining pattern.
        double t = frame_ / static_cast<double>(kProcessFrames);
        mx_      = kZoneB + 60.0 * std::sin(t * 6.283 * 2.0);
        my_      = 40.0 * std::sin(t * 6.283 * 3.0);
        if (++frame_ >= kProcessFrames)
        {
            mx_    = kZoneB;
            my_    = 0.0;
            state_ = 3; // done — BC may now pick it up
            return UF_NEXT(OnRun_Idle);
        }
        return Sleep(kFrame);
    }

    int    state_ = 0;
    int    frame_ = 0;
    double mx_    = kZoneB;
    double my_    = 0.0;
};

// ════════════════════════════════════════════════════════════════════════════
//  Picker — one class, two instances. AB carries A->B, BC carries B->C.
// ════════════════════════════════════════════════════════════════════════════
class Picker : public uniflow::Uniflow<Picker>
{
    UF_USES_UNIFLOW(Picker);

public:
    enum class Role { LoadToStage, StageToUnload };

    // Entry step — the role decides this picker's zones and partner.
    StepResult OnRun_Begin(Role role)
    {
        role_ = role;
        if (role == Role::LoadToStage)
        {
            src_x_ = kZoneA; dst_x_ = kZoneB; home_x_ = kZoneA;
            other_ = "BC";
        }
        else
        {
            src_x_ = kZoneB; dst_x_ = kZoneC; home_x_ = kZoneC;
            other_ = "AB";
        }
        x_ = home_x_;
        return UF_NEXT(OnCycle);
    }

    // ── Read by the Viz module and the partner picker (same pump thread) ──
    double      x() const { return x_; }
    double      z() const { return z_; }
    bool        carrying() const { return carry_; }
    const char* phase() const { return phase_; }

private:
    StepResult OnCycle()
    {
        if (g_stop)
            return Done();
        phase_ = "wait part";
        return UF_NEXT(OnWaitSource);
    }

    // Wait until there is something to pick up at the source zone.
    StepResult OnWaitSource()
    {
        if (g_stop)
            return Done();
        if (!SourceReady())
            return Sleep(kFrame);
        return UF_NEXT(OnGoSource);
    }

    StepResult OnGoSource()
    {
        if (g_stop)
            return Done();
        phase_ = "-> source";
        // BC's source IS the B zone, so entering it needs B clear.
        bool clear = (role_ == Role::StageToUnload)
                         ? (Stage::inst().CanPickup() && !PartnerInB())
                         : true;
        if (MoveX(src_x_, clear))
            return UF_NEXT(OnPickDown);
        return Sleep(kFrame);
    }

    StepResult OnPickDown()
    {
        if (MoveZ(kZDown))
        {
            // Grab the part: clear the source, mark this picker loaded.
            if (role_ == Role::LoadToStage)
                g_zoneA_part = false;
            else
                Stage::inst().Pickup();
            carry_ = true;
            return UF_NEXT(OnPickUp);
        }
        phase_ = "pick v";
        return Sleep(kFrame);
    }

    StepResult OnPickUp()
    {
        phase_ = "pick ^";
        if (MoveZ(kZUp))
            return UF_NEXT(OnGoDest);
        return Sleep(kFrame);
    }

    StepResult OnGoDest()
    {
        if (g_stop)
            return Done();
        phase_ = "-> dest";
        // AB's destination is the B zone — enter it only when the Stage is
        // free AND the partner has cleared B. This is the collision rule.
        bool clear = (role_ == Role::LoadToStage)
                         ? (Stage::inst().CanReceive() && !PartnerInB())
                         : true;
        if (!clear && !InB(x_))
            phase_ = "wait B";
        if (MoveX(dst_x_, clear))
            return UF_NEXT(OnPlaceDown);
        return Sleep(kFrame);
    }

    StepResult OnPlaceDown()
    {
        if (MoveZ(kZDown))
        {
            // Release the part at the destination zone.
            if (role_ == Role::LoadToStage)
                Stage::inst().Receive();
            else
                ++g_delivered;
            carry_ = false;
            return UF_NEXT(OnPlaceUp);
        }
        phase_ = "place v";
        return Sleep(kFrame);
    }

    StepResult OnPlaceUp()
    {
        phase_ = "place ^";
        if (MoveZ(kZUp))
            return UF_NEXT(OnRetreat);
        return Sleep(kFrame);
    }

    // Retreat to the home position — this is what clears the B zone for the
    // partner picker.
    StepResult OnRetreat()
    {
        if (g_stop)
            return Done();
        phase_ = "retreat";
        if (MoveX(home_x_, /*clear=*/true))
            return UF_NEXT(OnCycle);
        return Sleep(kFrame);
    }

    // ── helpers ──
    bool SourceReady() const
    {
        return (role_ == Role::LoadToStage) ? g_zoneA_part
                                            : Stage::inst().CanPickup();
    }
    // Is the partner picker currently occupying the B zone?
    bool PartnerInB() const { return InB(Picker::GetInst(other_).x()); }

    // Move x one frame toward `target`. A move that would ENTER the B zone is
    // held back unless `clear_to_enter_B` is true. Returns true on arrival.
    bool MoveX(double target, bool clear_to_enter_B)
    {
        double dir = (target > x_) ? 1.0 : -1.0;
        double nx  = x_ + dir * kXSpeed;
        if ((dir > 0 && nx > target) || (dir < 0 && nx < target))
            nx = target;
        if (!InB(x_) && InB(nx) && !clear_to_enter_B)
            return false; // hold outside B — no collision allowed
        x_ = nx;
        return x_ == target;
    }
    bool MoveZ(double target)
    {
        double dir = (target > z_) ? 1.0 : -1.0;
        double nz  = z_ + dir * kZSpeed;
        if ((dir > 0 && nz > target) || (dir < 0 && nz < target))
            nz = target;
        z_ = nz;
        return z_ == target;
    }

    Role        role_   = Role::LoadToStage;
    double      x_      = kZoneA;
    double      z_      = kZUp;
    bool        carry_  = false;
    double      src_x_  = kZoneA;
    double      dst_x_  = kZoneB;
    double      home_x_ = kZoneA;
    const char* other_  = "BC";
    const char* phase_  = "-";
};

// ════════════════════════════════════════════════════════════════════════════
//  Loader — drops a raw part at zone A at random intervals. Single instance.
// ════════════════════════════════════════════════════════════════════════════
class Loader : public uniflow::Uniflow<Loader>
{
    UF_SINGLETON(Loader);

public:
    StepResult OnRun_Begin() { return UF_NEXT(OnArm); }

private:
    StepResult OnArm()
    {
        if (g_stop)
            return Done();
        std::uniform_int_distribution<int> d(400, 1200);
        due_ = uniflow::Clock::now() + std::chrono::milliseconds(d(rng_));
        return UF_NEXT(OnWait);
    }
    StepResult OnWait()
    {
        if (g_stop)
            return Done();
        if (uniflow::Clock::now() < due_)
            return Sleep(kFrame);
        // Drop a part only if Load is free; otherwise wait for AB to take it.
        if (!g_zoneA_part)
        {
            g_zoneA_part = true;
            return UF_NEXT(OnArm);
        }
        return Sleep(kFrame);
    }

    std::mt19937            rng_{12345};
    uniflow::TimePoint      due_;
};

// ════════════════════════════════════════════════════════════════════════════
//  Viz — snapshots the whole line into g_snap every frame. It only reads other
//  modules (safe — same pump thread) and writes the snapshot under one mutex.
// ════════════════════════════════════════════════════════════════════════════
class Viz : public uniflow::Uniflow<Viz>
{
    UF_SINGLETON(Viz);

public:
    StepResult OnRun_Begin() { return UF_NEXT(OnTick); }

private:
    StepResult OnTick()
    {
        if (g_stop)
            return Done();
        const Picker& ab = Picker::GetInst("AB");
        const Picker& bc = Picker::GetInst("BC");
        const Stage&  st = Stage::inst();
        {
            std::lock_guard<std::mutex> lk(g_snap_mu);
            g_snap.ab_x       = ab.x();
            g_snap.ab_z       = ab.z();
            g_snap.ab_carry   = ab.carrying();
            g_snap.ab_phase   = ab.phase();
            g_snap.bc_x       = bc.x();
            g_snap.bc_z       = bc.z();
            g_snap.bc_carry   = bc.carrying();
            g_snap.bc_phase   = bc.phase();
            g_snap.stage_x    = st.mx();
            g_snap.stage_y    = st.my();
            g_snap.stage_state = st.state();
            g_snap.zoneA_part = g_zoneA_part;
            g_snap.delivered  = g_delivered;
        }
        return Sleep(kFrame);
    }
};

// Read a consistent copy of the line state.
static Snapshot ReadSnapshot()
{
    std::lock_guard<std::mutex> lk(g_snap_mu);
    return g_snap;
}

// Start every module's flow. Called once before the UI loop.
static void StartLine()
{
    Stage::inst().Start(&Stage::OnRun_Begin);
    Loader::inst().Start(&Loader::OnRun_Begin);
    Viz::inst().Start(&Viz::OnRun_Begin);
    Picker::GetInst("AB").Start(&Picker::OnRun_Begin, Picker::Role::LoadToStage);
    Picker::GetInst("BC").Start(&Picker::OnRun_Begin, Picker::Role::StageToUnload);
}

// Tell every module to wind down, then wait for the pump to go quiet.
static void StopLine()
{
    g_stop.store(true);
    Picker::GetInst("AB").Wait();
    Picker::GetInst("BC").Wait();
    Stage::inst().Wait();
    Loader::inst().Wait();
    Viz::inst().Wait();
}

// ════════════════════════════════════════════════════════════════════════════
//  Visualisation
// ════════════════════════════════════════════════════════════════════════════
#if UNIFLOW_WIN32_VIZ

static int SX(double x) { return 60 + static_cast<int>(x / kXMax * 860.0); }
static int SZ(double z) { return 96 + static_cast<int>(z / kZDown * 210.0); }

static void DrawScene(HDC hdc, const RECT& rc)
{
    // Double-buffer into a memory DC to avoid flicker.
    HDC     mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HBITMAP old = static_cast<HBITMAP>(SelectObject(mem, bmp));

    HBRUSH bg = CreateSolidBrush(RGB(24, 26, 32));
    FillRect(mem, &rc, bg);
    DeleteObject(bg);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB(210, 214, 222));

    Snapshot s = ReadSnapshot();

    // Rails the two pickers ride on (AB slightly above BC so they never
    // visually overlap — the "살짝 비켜있어야" offset).
    HPEN rail = CreatePen(PS_SOLID, 3, RGB(70, 74, 84));
    HPEN oldp = static_cast<HPEN>(SelectObject(mem, rail));
    MoveToEx(mem, 50, 70, nullptr); LineTo(mem, 930, 70);
    MoveToEx(mem, 50, 86, nullptr); LineTo(mem, 930, 86);
    SelectObject(mem, oldp);
    DeleteObject(rail);

    // Zone floors.
    auto zone = [&](double zx, const char* label, COLORREF c)
    {
        RECT   z{SX(zx) - 56, 330, SX(zx) + 56, 372};
        HBRUSH b = CreateSolidBrush(c);
        FillRect(mem, &z, b);
        DeleteObject(b);
        TextOutA(mem, SX(zx) - 50, 376, label, lstrlenA(label));
    };
    zone(kZoneA, "A  Load", RGB(40, 60, 90));
    zone(kZoneB, "B  Stage", RGB(70, 55, 90));
    zone(kZoneC, "C  Unload", RGB(40, 80, 60));

    // B-zone safety span — the region only one picker may occupy.
    HPEN span = CreatePen(PS_DOT, 1, RGB(120, 90, 130));
    oldp      = static_cast<HPEN>(SelectObject(mem, span));
    MoveToEx(mem, SX(kZoneB - kBClear), 60, nullptr);
    LineTo(mem, SX(kZoneB - kBClear), 330);
    MoveToEx(mem, SX(kZoneB + kBClear), 60, nullptr);
    LineTo(mem, SX(kZoneB + kBClear), 330);
    SelectObject(mem, oldp);
    DeleteObject(span);

    // The Stage table at B (shifts with its x/y while machining).
    {
        int    sx = SX(s.stage_x);
        int    sy = 318 + static_cast<int>(s.stage_y / 40.0 * 8.0);
        RECT   t{sx - 34, sy - 12, sx + 34, sy + 12};
        COLORREF c = s.stage_state == 2 ? RGB(200, 120, 60)
                     : s.stage_state == 3 ? RGB(90, 170, 110)
                                          : RGB(90, 94, 104);
        HBRUSH b = CreateSolidBrush(c);
        FillRect(mem, &t, b);
        DeleteObject(b);
    }

    // A raw part waiting at Load.
    if (s.zoneA_part)
    {
        RECT   p{SX(kZoneA) - 11, 308, SX(kZoneA) + 11, 330};
        HBRUSH b = CreateSolidBrush(RGB(210, 180, 70));
        FillRect(mem, &p, b);
        DeleteObject(b);
    }

    // Draw one picker: a head on its arm, a part square when carrying.
    auto picker = [&](double px, double pz, bool carry, int rail_y,
                      COLORREF c, const char* tag, const std::string& phase)
    {
        int  hx = SX(px), hy = SZ(pz);
        HPEN arm = CreatePen(PS_SOLID, 5, c);
        oldp     = static_cast<HPEN>(SelectObject(mem, arm));
        MoveToEx(mem, hx, rail_y, nullptr);
        LineTo(mem, hx, hy);
        SelectObject(mem, oldp);
        DeleteObject(arm);

        HBRUSH hb = CreateSolidBrush(c);
        RECT   head{hx - 16, hy - 9, hx + 16, hy + 9};
        FillRect(mem, &head, hb);
        DeleteObject(hb);

        if (carry)
        {
            RECT   part{hx - 10, hy + 9, hx + 10, hy + 29};
            HBRUSH pb = CreateSolidBrush(RGB(210, 180, 70));
            FillRect(mem, &part, pb);
            DeleteObject(pb);
        }
        std::string label = std::string(tag) + " " + phase;
        TextOutA(mem, hx - 34, rail_y - 18, label.c_str(),
                 static_cast<int>(label.size()));
    };
    picker(s.ab_x, s.ab_z, s.ab_carry, 70, RGB(90, 150, 230), "AB", s.ab_phase);
    picker(s.bc_x, s.bc_z, s.bc_carry, 86, RGB(230, 130, 90), "BC", s.bc_phase);

    // Header / counter.
    const char* hdr = "uniflow CNC line  -  one Picker class, two instances, "
                      "lock-free B-zone hand-off";
    TextOutA(mem, 50, 16, hdr, lstrlenA(hdr));
    std::string done = "delivered at C: " + std::to_string(s.delivered);
    TextOutA(mem, 760, 16, done.c_str(), static_cast<int>(done.size()));

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

static void RunVisualisation()
{
    const char* cls = "uniflow_cnc";
    WNDCLASSA   wc{};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandle(nullptr);
    wc.lpszClassName = cls;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassA(&wc);

    HWND hwnd = CreateWindowA(cls, "uniflow - CNC pickers",
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

#else // ── non-Windows: console animation ──────────────────────────────────

static void RunVisualisation()
{
    auto cell = [](double x) { return static_cast<int>(x / kXMax * 64.0); };
    const auto t0 = std::chrono::steady_clock::now();
    std::cout << "uniflow CNC line (console). Runs ~20s.\n";
    while (std::chrono::steady_clock::now() - t0 < 20s)
    {
        Snapshot s = ReadSnapshot();
        std::string track(66, ' ');
        track[cell(kZoneA)] = 'A';
        track[cell(kZoneB)] = 'B';
        track[cell(kZoneC)] = 'C';
        std::string ab(66, ' '), bc(66, ' ');
        ab[cell(s.ab_x)] = s.ab_carry ? '#' : 'v';
        bc[cell(s.bc_x)] = s.bc_carry ? '#' : 'v';
        std::cout << "\r AB[" << ab << "] " << s.ab_phase << "        \n"
                  << " BC[" << bc << "] " << s.bc_phase << "        \n"
                  << "   [" << track << "]  stage=" << s.stage_state
                  << " delivered=" << s.delivered << "   \x1b[3A" << std::flush;
        std::this_thread::sleep_for(80ms);
    }
    std::cout << "\x1b[3B\n";
}

#endif

// ════════════════════════════════════════════════════════════════════════════
int main()
{
    // The window/console IS the output — keep the per-step console log quiet.
    uniflow::SetObserver(std::make_unique<uniflow::IUniflowObserver>());

    // Two instances of ONE Picker class — named, because there are two.
    Picker ab{"AB"};
    Picker bc{"BC"};

    StartLine();
    RunVisualisation(); // blocks until the window closes (or the console timer)
    StopLine();

    std::cout << "parts delivered to Unload: " << g_delivered << "\n";
    return 0;
}
