// uf_visualization.cpp - pump-side snapshot writer + ANSI console dashboard.
//
// The snapshot step runs on the pump thread; RunVisualisation() draws on a
// background thread and reads g_snap under g_snap_mu. Console-only: no Win32.
#include "uf_visualization.h"

#include "app.h"
#include "console.h"
#include "mailbox.h"
#include "snapshot.h"
#include "uf_receiver.h"
#include "uf_sender.h"

#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

using namespace uniflow;
using namespace std::chrono_literals;

Flow_Visualization::Flow_Visualization(uniflow::Runtime& rt)
    : uniflow::Uniflow<Flow_Visualization>(rt, "Flow_Visualization")
{
    AddTask(task_snapshot_);
}

// Pump-side: copy live sender / receiver / mailbox state into g_snap under
// g_snap_mu, so the render thread always sees a consistent frame. This is a
// perpetual poll - it ends only on Stop.
StepResult Flow_Visualization::Task_Snapshot::Step1_Tick()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    const auto& s = App::inst().send;
    const auto& r = App::inst().recv;

    // Measure the per-job cycle speed: how long the receiver takes between
    // consecutive job completions, plus the overall average over the run.
    const auto now = std::chrono::steady_clock::now();
    if (!started_)
    {
        start_     = now;
        last_job_  = now;
        started_   = true;
    }
    const int processed = r.Processed();
    if (processed > last_processed_)
    {
        const double dt = std::chrono::duration<double, std::milli>(now - last_job_).count();
        last_cycle_ms_  = dt / (processed - last_processed_);
        last_job_       = now;
        last_processed_ = processed;
    }
    const double elapsed_ms = std::chrono::duration<double, std::milli>(now - start_).count();
    const double avg_ms     = processed > 0 ? elapsed_ms / processed : 0.0;
    const double jps        = last_cycle_ms_ > 0.0 ? 1000.0 / last_cycle_ms_ : 0.0;

    {
        std::lock_guard<std::mutex> lk(g_snap_mu);
        g_snap.last_cycle_ms    = last_cycle_ms_;
        g_snap.jobs_per_sec     = jps;
        g_snap.avg_ms_per_job   = avg_ms;
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

// ----- render side (background thread) -----

namespace
{

const std::string kSep = "  " + std::string(60, '-');

std::string FormatMsg(const Msg& m)
{
    std::ostringstream os;
    os << m.a << ' ' << m.op << ' ' << m.b;
    return os.str();
}

std::string StateColor(RecvState s)
{
    switch (s)
    {
    case RecvState::Adding:      return console::Fg(90, 180, 120);
    case RecvState::Subtracting: return console::Fg(210, 130, 60);
    case RecvState::Dispatching: return console::Fg(200, 190, 80);
    case RecvState::Idle:        return console::Fg(120, 124, 134);
    }
    return console::kReset;
}

void DrawConsole(const Snapshot& s)
{
    std::ostringstream out;

    auto put = [&](int row, const std::string& text)
    {
        out << console::At(row, 1) << console::kClearLine << text;
    };

    put(1, std::string("  ") + console::kBold + "uniflow queue_drain  "
               + console::kReset + console::kDim + "v" + uniflow::kVersion
               + console::kReset);
    put(2, kSep);

    // sender line: burst counters + current step description.
    {
        std::ostringstream line;
        line << "  " << console::kCyan << "sender" << console::kReset
             << "   bursts " << console::kBold << s.total_bursts << "/"
             << GlobalConfig::kMaxBurstCount << console::kReset
             << "   last burst " << s.last_burst_count
             << "   " << console::kDim << s.sender_phase << console::kReset;
        put(3, line.str());
    }

    // source vectors the sender draws operands from.
    {
        std::ostringstream a;
        std::ostringstream b;
        a << "  vec A:";
        b << "  vec B:";
        for (std::size_t i = 0; i < s.vec_a.size(); ++i)
        {
            a << ' ' << std::string(2 - std::to_string(s.vec_a[i]).size(), ' ')
              << s.vec_a[i];
        }
        for (std::size_t i = 0; i < s.vec_b.size(); ++i)
        {
            b << ' ' << std::string(2 - std::to_string(s.vec_b[i]).size(), ' ')
              << s.vec_b[i];
        }
        put(4, console::kGray + a.str() + console::kReset);
        put(5, console::kGray + b.str() + console::kReset);
    }

    put(6, kSep);

    // queue depth + chip view of the pending jobs (lock-free deque snapshot).
    {
        std::ostringstream line;
        line << "  " << console::kYellow << "queue" << console::kReset
             << "    depth " << console::kBold << s.queue.size()
             << console::kReset;
        put(7, line.str());

        std::ostringstream chips;
        chips << "  ";
        std::size_t shown = 0;
        for (; shown < s.queue.size() && shown < 12; ++shown)
        {
            chips << "[" << FormatMsg(s.queue[shown]) << "] ";
        }
        if (s.queue.size() > shown)
        {
            chips << "+" << (s.queue.size() - shown) << " more";
        }
        if (s.queue.empty())
        {
            chips << console::kDim << "(empty)" << console::kReset;
        }
        put(8, chips.str());
    }

    put(9, kSep);

    // receiver line: state chip + processed count + current step description.
    {
        std::ostringstream line;
        line << "  " << StateColor(s.recv_state) << "receiver "
             << ToString(s.recv_state) << console::kReset
             << "   processed " << console::kBold << s.processed
             << console::kReset << "   " << console::kDim << s.recv_phase
             << console::kReset;
        put(10, line.str());

        std::ostringstream last;
        last << "  last result: " << console::kBold
             << (s.last_result.empty() ? "-" : s.last_result) << console::kReset;
        put(11, last.str());
    }

    // cycle speed: how fast the receiver drains one job at a time.
    {
        std::ostringstream line;
        line << "  " << console::kCyan << "cycle" << console::kReset
             << "    last " << console::kBold << std::fixed << std::setprecision(1)
             << s.last_cycle_ms << " ms/job" << console::kReset
             << "   " << std::setprecision(1) << s.jobs_per_sec << " jobs/s"
             << "   " << console::kDim << "avg " << std::setprecision(1)
             << s.avg_ms_per_job << " ms/job" << console::kReset;
        put(12, line.str());
    }

    put(13, kSep);
    put(14, std::string("  ") + console::kDim + "press Enter to quit"
                + console::kReset);

    std::cout << out.str() << std::flush;
}

constexpr int kStatusRow = 15;

}  // namespace

void RunVisualisation()
{
    console::EnableAnsi();
    console::HideCursor();
    console::Clear();

    // Render on a background thread; the main thread blocks on stdin so a single
    // Enter quits. The render thread only READS the snapshot (mutex-guarded).
    std::atomic<bool> done{false};
    std::thread       render([&]
    {
        while (!done.load() && !GlobalEnv::Stop())
        {
            DrawConsole(ReadSnapshot());
            std::this_thread::sleep_for(40ms);   // ~25 fps
        }
    });

    std::string line;
    std::getline(std::cin, line);   // any Enter (or EOF) quits
    done.store(true);
    render.join();

    console::ShowCursor();
    std::cout << console::At(kStatusRow, 1) << console::kClearLine
              << "  queue_drain stopped.\n";
}
