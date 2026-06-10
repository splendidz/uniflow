// Feature test for uniflow Post / PostAndWait / Link.
//
// Self-verifying console test: prints [PASS]/[FAIL] per check and a summary.
// Exit code is 0 iff every check passed, so it doubles as a CI gate.
//
// Build: open feature_post_link.vcxproj in VS2022 and run (F5), or from a
// Developer Command Prompt:
//   cl /std:c++17 /EHsc /I ..\.. main.cpp /Fe:feature_post_link.exe
#include "uniflow.hpp"

#include <atomic>
#include <future>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
int g_checks   = 0;
int g_failures = 0;

void check(bool cond, const std::string& label)
{
    ++g_checks;
    if (cond)
    {
        std::cout << "  [PASS] " << label << "\n";
    }
    else
    {
        ++g_failures;
        std::cout << "  [FAIL] " << label << "\n";
    }
}

// Silent observer: keeps the default per-step console log out of the test
// output so PASS/FAIL lines stand alone.
class SilentObserver : public uniflow::IUniflowObserver
{
};

// Counts observer callbacks so a test can prove which runtime's observer saw
// a given flow (identity preservation under Link).
class CountingObserver : public uniflow::IUniflowObserver
{
public:
    std::atomic<int> step_changes{0};
    std::atomic<int> flows_ended{0};

    void OnStepChanged(std::string_view, std::string_view, std::string_view,
                       std::string_view, int, double,
                       const uniflow::TickStats&) override
    {
        step_changes.fetch_add(1);
    }
    void OnFlowEnded(std::string_view, uniflow::StepAction, int,
                     const std::vector<uniflow::TraceEntry>&, double, double,
                     double, const uniflow::FlowTickSummary&,
                     const uniflow::FlowStats&, uniflow::FlowOrigin) override
    {
        flows_ended.fetch_add(1);
    }
};

uniflow::Runtime::Opts silent_opts()
{
    uniflow::Runtime::Opts o;
    o.observer = std::make_unique<SilentObserver>();
    return o;
}

// Records the cross-runtime observer hooks and the caller info they carry, so
// a test can prove Post / PostAndWait / Link are logged with their source
// location. Touched from both the calling thread and the pump thread.
class PostLinkObserver : public uniflow::IUniflowObserver
{
public:
    std::mutex  mu;
    int         submitted = 0;
    int         executed  = 0;
    int         linked    = 0;
    std::string last_submit_file;
    int         last_submit_line = 0;
    std::string last_submit_func;
    std::string last_link_file;

    void OnPostSubmitted(int, bool, uniflow::CallSite c) override
    {
        std::lock_guard<std::mutex> lk(mu);
        ++submitted;
        if (c.file) last_submit_file = c.file;
        last_submit_line = c.line;
        if (c.function) last_submit_func = c.function;
    }
    void OnPostExecuted(int, bool, double, uniflow::CallSite) override
    {
        std::lock_guard<std::mutex> lk(mu);
        ++executed;
    }
    void OnLinked(int, int, uniflow::CallSite c) override
    {
        std::lock_guard<std::mutex> lk(mu);
        ++linked;
        if (c.file) last_link_file = c.file;
    }
};
} // namespace

// Shared state touched only from a single pump thread - never locked.
namespace shared
{
std::vector<int> g_order;
int              g_counter = 0;
} // namespace shared

// Holds the pump thread for ~15ms in a single step - simulates the "a whole
// cycle suddenly took long" scenario the round profiler is meant to catch.
// Busy-waits rather than sleeps so the time is real pump-thread work.
class SlowStep : public uniflow::Uniflow<SlowStep>
{
    UF_UNIFLOW_IMPLEMENT(SlowStep);

public:
    explicit SlowStep(uniflow::Runtime& rt) : uniflow::Uniflow<SlowStep>(rt) {}

    StepResult OnSlow_Begin()
    {
        const auto t0 = uniflow::Clock::now();
        while (uniflow::Clock::now() - t0 < std::chrono::milliseconds(15))
        {
            // spin - deliberately monopolise the pump
        }
        return Done();
    }
};

// Trivial fast flow used to generate fresh, sub-millisecond rounds after a
// stats reset.
class FastStep : public uniflow::Uniflow<FastStep>
{
    UF_UNIFLOW_IMPLEMENT(FastStep);

public:
    explicit FastStep(uniflow::Runtime& rt) : uniflow::Uniflow<FastStep>(rt) {}

    StepResult OnFast_Begin() { return Done(); }
};

// Captures the slow-round report so a test can prove the culprit segment is
// named with its duration.
class RoundObserver : public uniflow::IUniflowObserver
{
public:
    std::mutex  mu;
    int         slow_rounds   = 0;
    double      last_busy_ms  = 0.0;
    int         last_segments = 0;
    std::string last_step_obj;
    double      last_step_ms = 0.0;

    void OnSlowRound(int, const uniflow::RoundProfile& p) override
    {
        std::lock_guard<std::mutex> lk(mu);
        ++slow_rounds;
        last_busy_ms  = p.busy_ms;
        last_segments = static_cast<int>(p.segments.size());
        for (const auto& s : p.segments)
            if (s.kind == uniflow::RoundSegment::Kind::Step)
            {
                last_step_obj = s.obj;
                last_step_ms  = s.ms;
            }
    }
};

// Records the thread it runs on, then counts a shared int up to 'target'.
class Counter : public uniflow::Uniflow<Counter>
{
    UF_UNIFLOW_IMPLEMENT(Counter);

public:
    explicit Counter(uniflow::Runtime& rt) : uniflow::Uniflow<Counter>(rt) {}

    std::thread::id ran_on{};
    int             target = 5;

    StepResult OnCount_Begin()
    {
        ran_on = std::this_thread::get_id();
        return UF_NEXT(OnCount_Loop);
    }

private:
    StepResult OnCount_Loop()
    {
        if (shared::g_counter >= target)
            return Done();
        ++shared::g_counter;
        return Stay();
    }
};

int main()
{
    const std::thread::id main_id = std::this_thread::get_id();

    // ---- Test 1: Post (fire-and-forget) runs on the pump thread ----
    {
        std::cout << "Test 1: Post (fire-and-forget) runs on the pump thread\n";
        uniflow::Runtime rt(silent_opts());

        std::promise<std::thread::id> p;
        std::future<std::thread::id>  fut = p.get_future();
        rt.Post([&p] { p.set_value(std::this_thread::get_id()); });
        std::thread::id where = fut.get();

        check(where != main_id, "Post callback ran off the calling thread");
    }

    // ---- Test 2: PostAndWait returns a value computed on the pump ----
    {
        std::cout << "Test 2: PostAndWait returns a value computed on the pump\n";
        uniflow::Runtime rt(silent_opts());

        int sum = rt.PostAndWait([] { return 40 + 2; }).get();
        check(sum == 42, "PostAndWait returned the callback's value");

        std::thread::id where =
            rt.PostAndWait([] { return std::this_thread::get_id(); }).get();
        check(where != main_id, "PostAndWait callback ran off the calling thread");
    }

    // ---- Test 3: Post preserves FIFO submission order ----
    {
        std::cout << "Test 3: Post preserves FIFO order\n";
        uniflow::Runtime rt(silent_opts());

        shared::g_order.clear();
        for (int i = 0; i < 5; ++i)
            rt.Post([i] { shared::g_order.push_back(i); });

        std::vector<int> snapshot =
            rt.PostAndWait([] { return shared::g_order; }).get();

        bool ok = snapshot.size() == 5;
        for (int i = 0; ok && i < 5; ++i)
            ok = (snapshot[static_cast<std::size_t>(i)] == i);
        check(ok, "posted callbacks ran in submission order");
    }

    // ---- Test 4: Link drives sub-runtime modules on the driver pump ----
    {
        std::cout << "Test 4: Link drives sub-runtime modules on the driver pump\n";
        uniflow::Runtime rt(silent_opts());
        uniflow::Runtime sub_rt(silent_opts());

        shared::g_counter = 0;
        Counter c{sub_rt};
        c.target = 10;

        // sub_rt's own pump thread id, sampled BEFORE linking.
        std::thread::id sub_pump_before =
            sub_rt.PostAndWait([] { return std::this_thread::get_id(); }).get();

        rt.Link(sub_rt); // sub_rt's pump stops; rt's pump takes over its modules

        std::thread::id driver_pump =
            rt.PostAndWait([] { return std::this_thread::get_id(); }).get();

        UF_START_FLOW(c, OnCount_Begin);
        c.WaitUntilIdle();

        check(shared::g_counter == 10, "linked module flow ran to completion");
        check(c.ran_on == driver_pump,
              "linked module ran on the driver's pump thread");
        check(c.ran_on != sub_pump_before,
              "linked module no longer runs on its original pump");
    }

    // ---- Test 5: Link preserves each runtime's own observer ----
    {
        std::cout << "Test 5: Link preserves each runtime's own observer\n";
        auto rt_obs_owned  = std::make_unique<CountingObserver>();
        auto sub_obs_owned = std::make_unique<CountingObserver>();
        CountingObserver* rt_obs  = rt_obs_owned.get();
        CountingObserver* sub_obs = sub_obs_owned.get();

        uniflow::Runtime::Opts od;
        od.observer = std::move(rt_obs_owned);
        uniflow::Runtime rt(std::move(od));

        uniflow::Runtime::Opts os;
        os.observer = std::move(sub_obs_owned);
        uniflow::Runtime sub_rt(std::move(os));

        shared::g_counter = 0;
        Counter c{sub_rt};
        c.target = 3;

        rt.Link(sub_rt);
        UF_START_FLOW(c, OnCount_Begin);
        c.WaitUntilIdle();

        check(sub_obs->flows_ended.load() == 1,
              "sub_rt's own observer saw the flow end");
        check(rt_obs->flows_ended.load() == 0,
              "driver's observer did NOT see sub_rt's flow");
    }

    // ---- Test 6: PostAndWait to a linked runtime is serviced by the driver ----
    {
        std::cout << "Test 6: PostAndWait to a linked runtime hits the driver pump\n";
        uniflow::Runtime rt(silent_opts());
        uniflow::Runtime sub_rt(silent_opts());

        rt.Link(sub_rt);

        std::thread::id driver_pump =
            rt.PostAndWait([] { return std::this_thread::get_id(); }).get();
        std::thread::id via_sub =
            sub_rt.PostAndWait([] { return std::this_thread::get_id(); }).get();

        check(via_sub == driver_pump,
              "posting to the linked runtime ran on the driver pump");
    }

    // ---- Test 7: observer logs Post / PostAndWait / Link with caller info ----
    {
        std::cout << "Test 7: observer logs Post / PostAndWait / Link with caller\n";
        auto              obs_owned = std::make_unique<PostLinkObserver>();
        PostLinkObserver* obs       = obs_owned.get();
        uniflow::Runtime::Opts od;
        od.observer = std::move(obs_owned);
        uniflow::Runtime rt(std::move(od));

        UF_POST(rt, [] { /* fire-and-forget */ });
        int v = UF_POST_WAIT(rt, [] { return 7; }).get(); // forces a pump round

        uniflow::Runtime sub_rt(silent_opts());
        UF_LINK(rt, sub_rt);

        int         submitted, executed, linked, line;
        std::string sfile, sfunc, lfile;
        {
            std::lock_guard<std::mutex> lk(obs->mu);
            submitted = obs->submitted;
            executed  = obs->executed;
            linked    = obs->linked;
            line      = obs->last_submit_line;
            sfile     = obs->last_submit_file;
            sfunc     = obs->last_submit_func;
            lfile     = obs->last_link_file;
        }

        check(v == 7, "UF_POST_WAIT returned the callback value");
        check(submitted == 2, "OnPostSubmitted fired for both posts");
        check(executed >= 1, "OnPostExecuted fired on the pump");
        check(linked == 1, "OnLinked fired for the link");
        check(sfile == "main.cpp", "post caller file basename captured");
        check(line > 0, "post caller line captured");
        check(sfunc == "main", "post caller function captured");
        check(lfile == "main.cpp", "link caller file basename captured");
    }

    // ---- Test 8: slow-round profiling + round stats + reset ----
    {
        std::cout << "Test 8: slow-round profiling reports the culprit + stats reset\n";
        auto           obs_owned = std::make_unique<RoundObserver>();
        RoundObserver* obs       = obs_owned.get();
        uniflow::Runtime::Opts od;
        od.observer                       = std::move(obs_owned);
        od.config.slow_round_threshold_ms = std::chrono::milliseconds(5);
        od.config.trace_rounds            = true; // heavy per-segment tracing on
        uniflow::Runtime rt(std::move(od));

        SlowStep s{rt};
        UF_START_FLOW(s, OnSlow_Begin);
        s.WaitUntilIdle();
        // Barrier: a post runs in a round AFTER the slow round, guaranteeing
        // the slow round's RecordRound (stats + OnSlowRound) has completed
        // before we read anything.
        rt.PostAndWait([] { return 0; }).get();

        int         slow, segs;
        double      busy, sms;
        std::string sobj;
        {
            std::lock_guard<std::mutex> lk(obs->mu);
            slow = obs->slow_rounds;
            busy = obs->last_busy_ms;
            segs = obs->last_segments;
            sobj = obs->last_step_obj;
            sms  = obs->last_step_ms;
        }
        uniflow::RoundStats st = rt.GetRoundStats();

        check(slow >= 1, "OnSlowRound fired for the slow cycle");
        check(busy >= 10.0, "slow-round busy time captured (>=10ms)");
        check(segs >= 1 && !sobj.empty(), "per-segment breakdown populated");
        check(sobj == "SlowStep", "culprit segment names the module");
        check(sms >= 10.0, "culprit step duration captured (>=10ms)");
        check(st.max_ms >= 10.0, "round stats max captured the peak");
        check(st.count >= 1, "round stats counted the work round");

        // Reset clears the peak; subsequent activity is all sub-millisecond,
        // so the recorded max must drop well below the 15ms spike.
        rt.ResetRoundStats();
        FastStep f{rt};
        UF_START_FLOW(f, OnFast_Begin);
        f.WaitUntilIdle();
        rt.PostAndWait([] { return 0; }).get(); // barrier

        uniflow::RoundStats after = rt.GetRoundStats();
        check(after.max_ms < 5.0, "ResetRoundStats cleared the 15ms peak");
    }

    std::cout << "\n=== " << (g_checks - g_failures) << "/" << g_checks
              << " checks passed ===\n";
    return g_failures == 0 ? 0 : 1;
}
