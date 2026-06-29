#include "uf_visualization.h"

#include "app.h"
#include "console.h"
#include "globals.h"
#include "mailbox.h"
#include "snapshot.h"
#include "uf_friend.h"
#include "uf_professor.h"
#include "uf_student.h"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

using namespace uniflow;
using namespace std::chrono_literals;

// ======================================================================
//  Pump-side: snapshot the world into g_snap each round.
// ======================================================================

Flow_Visualization::Flow_Visualization(uniflow::Runtime& rt)
    : uniflow::Uniflow<Flow_Visualization>(rt, "Flow_Visualization")
{
    AddTask(task_snapshot_);
}

StepResult Flow_Visualization::Task_Snapshot::Step1_Tick()
{
    if (GlobalEnv::Stop())
    {
        Describe("viz stop");
        return Done();
    }

    // Plain member reads of every peer: all modules share this pump thread, so
    // there is no race and no lock needed for the reads themselves.
    const Flow_Professor& prof    = App::inst().prof;
    const Flow_Friend&    frnd    = App::inst().friend_;
    const Flow_Student&   stu     = App::inst().student;

    Snapshot s;
    s.prof_emitted = prof.Emitted();
    s.prof_total   = prof.Total();
    s.prof_idle    = prof.IsIdle();
    s.prof_phase   = prof.CurrentStepDescription();

    s.friend_emitted = frnd.Emitted();
    s.friend_total   = frnd.Total();
    s.friend_idle    = frnd.IsIdle();
    s.friend_phase   = frnd.CurrentStepDescription();

    s.student_current = stu.CurrentMessage();
    s.student_has_msg = !stu.IsIdle();
    s.student_ability = stu.Ability();
    s.student_stress  = stu.Stress();
    s.student_hours   = stu.HoursSpent();
    s.student_done    = stu.DoneCount();
    s.student_idle    = stu.IsIdle();
    s.student_phase   = stu.CurrentStepDescription();

    // Walk the mailbox without popping - safe, same pump thread.
    s.queue.reserve(Mailbox::Size());
    Mailbox::ForEach([&](const Message& m) { s.queue.push_back(m); });

    s.recent = GlobalLog::Recent(6);

    {
        std::lock_guard<std::mutex> lk(g_snap_mu);
        g_snap = std::move(s);
    }
    return Stay();   // re-poll every round to keep the snapshot fresh
}

// ======================================================================
//  Render thread: draw an ANSI dashboard from g_snap.
// ======================================================================

namespace
{

const std::string kSep = "  " + std::string(70, '-');

std::string FmtMsg(const Message& m)
{
    std::ostringstream os;
    if (m.kind == Message::Kind::Assignment)
    {
        os << "ASN  " << std::left << std::setw(10) << m.name
           << " need_ability=" << m.need_ability
           << " need_time=" << m.need_time << "h";
    }
    else
    {
        os << "PLAY " << std::left << std::setw(10) << m.name
           << " " << m.play_hours << "h";
    }
    return os.str();
}

const char* MsgColor(const Message& m)
{
    return m.kind == Message::Kind::Assignment ? console::kYellow : console::kGreen;
}

void Render(const Snapshot& s)
{
    std::ostringstream out;
    out << console::kSaveCursor;

    int row = 1;
    auto put = [&](const std::string& text)
    {
        out << console::At(row, 1) << console::kClearLine << text;
        ++row;
    };

    put(std::string("  ") + console::kBold
        + "uniflow message_dispatch  " + console::kReset
        + console::kDim + "v" + uniflow::kVersion + console::kReset);
    put(std::string("  ") + console::kDim
        + "professor + friend -> shared mailbox -> student   (one Runtime, one pump, lock-free)"
        + console::kReset);
    put(kSep);

    // ---- spawners ----
    {
        std::ostringstream ps;
        ps << "  " << console::kYellow << "Professor" << console::kReset
           << "  " << s.prof_emitted << "/" << s.prof_total << " sent"
           << (s.prof_total > 0 && s.prof_idle && s.prof_emitted >= s.prof_total
                   ? "  (done)" : "")
           << "   " << console::kDim << s.prof_phase << console::kReset;
        put(ps.str());

        std::ostringstream fs;
        fs << "  " << console::kGreen << "Friend   " << console::kReset
           << "  " << s.friend_emitted << "/" << s.friend_total << " sent"
           << (s.friend_total > 0 && s.friend_idle && s.friend_emitted >= s.friend_total
                   ? "  (done)" : "")
           << "   " << console::kDim << s.friend_phase << console::kReset;
        put(fs.str());
    }
    put(kSep);

    // ---- mailbox ----
    {
        std::ostringstream ms;
        ms << "  " << console::kBold << "Mailbox" << console::kReset
           << "  (" << s.queue.size() << " queued)";
        put(ms.str());
    }
    if (s.queue.empty())
    {
        put(std::string("    ") + console::kDim
            + "(empty - student resting or spawners between bursts)" + console::kReset);
    }
    else
    {
        const std::size_t kMaxShown = 8;
        for (std::size_t i = 0; i < s.queue.size() && i < kMaxShown; ++i)
        {
            const Message& m = s.queue[i];
            std::ostringstream ls;
            ls << "    " << MsgColor(m) << FmtMsg(m) << console::kReset;
            put(ls.str());
        }
        if (s.queue.size() > kMaxShown)
        {
            std::ostringstream more;
            more << "    " << console::kDim << "... and "
                 << (s.queue.size() - kMaxShown) << " more" << console::kReset;
            put(more.str());
        }
    }
    // Pad the mailbox region to a fixed height so the rows below do not jump.
    {
        int target = 7 + 9;   // header(3)+spawners(3)+sep+mailbox title.. fixed floor
        while (row < target)
        {
            put("");
        }
    }
    put(kSep);

    // ---- student ----
    {
        std::ostringstream hs;
        hs << "  " << console::kBold << "Student" << console::kReset
           << "   " << (s.student_idle ? std::string(console::kDim) + "idle" + console::kReset
                                       : std::string("active: ")
                                         + MsgColor(s.student_current)
                                         + FmtMsg(s.student_current) + console::kReset);
        put(hs.str());

        put(std::string("    ") + console::kDim + "phase: " + s.student_phase
            + console::kReset);

        std::ostringstream ab;
        ab << "    ability [" << console::kCyan
           << console::Bar(s.student_ability / 10.0, 20) << console::kReset
           << "] " << std::setw(2) << s.student_ability << "/10";
        put(ab.str());

        const char* sc = s.student_stress >= GlobalConfig::kStressMax - 1 ? console::kRed
                       : s.student_stress >= GlobalConfig::kStressMax / 2 ? console::kYellow
                       : console::kGreen;
        std::ostringstream st;
        st << "    stress  [" << sc
           << console::Bar(static_cast<double>(s.student_stress) / GlobalConfig::kStressMax, 20)
           << console::kReset << "] " << std::setw(2) << s.student_stress
           << "/" << GlobalConfig::kStressMax;
        put(st.str());

        std::ostringstream ct;
        ct << "    hours spent: " << s.student_hours
           << "        messages handled: " << s.student_done;
        put(ct.str());
    }
    put(kSep);

    // ---- recent activity ----
    put(std::string("  ") + console::kBold + "Recent activity" + console::kReset);
    {
        const std::size_t kLines = 6;
        std::size_t shown = 0;
        for (const std::string& line : s.recent)
        {
            put(std::string("    ") + console::kGray + line + console::kReset);
            ++shown;
        }
        while (shown < kLines)
        {
            put("");
            ++shown;
        }
    }
    put(kSep);
    put(std::string("  ") + console::kDim + "press Enter to quit" + console::kReset);

    out << console::kRestoreCursor;
    std::cout << out.str() << std::flush;
}

}  // namespace

void RunConsoleRenderer(std::atomic<bool>& quit)
{
    while (!quit.load())
    {
        Render(ReadSnapshot());
        std::this_thread::sleep_for(40ms);   // ~25 fps
    }
}
