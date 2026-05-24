// ======================================================================
//  env_log_observer.cpp - implementation of the wide-column logger.
// ======================================================================
#include "env_log_observer.h"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace {
    std::string Pad(std::string_view sv, int width)
    {
        std::string s(sv);
        if (static_cast<int>(s.size()) < width)
            s.append(width - s.size(), ' ');
        return s;
    }
    std::string Pad2(int v)
    {
        std::ostringstream os;
        os << std::setw(2) << std::setfill('0') << v;
        return os.str();
    }
    std::string FmtMs(uniflow::Duration d)
    {
        std::ostringstream os;
        os.setf(std::ios::fixed);
        os.precision(2);
        os << uniflow::to_ms(d) << "ms";
        return os.str();
    }
}

EnvLogObserver::EnvLogObserver(std::string file_path)
    : path_(std::move(file_path))
{}

void EnvLogObserver::OnFlowStarted(std::string_view obj,
                                   uniflow::FlowOrigin origin)
{
    std::ostringstream os;
    os << "FLOW START";
    if (origin.file)
        os << "  caller=" << origin.file << ":" << origin.line;
    Emit(obj, os.str());
}

void EnvLogObserver::OnStepRan(std::string_view obj, std::string_view step,
                               std::string_view description,
                               int step_ordinal, int tick,
                               uniflow::Duration elapsed_cpu)
{
    std::ostringstream os;
    os << Pad(step, kColStep) << " "
       << Pad(description, kColDesc) << " "
       << "#" << Pad2(step_ordinal) << "/#" << Pad2(tick)
       << " elapsed=" << FmtMs(elapsed_cpu);
    Emit(obj, os.str());
}

void EnvLogObserver::OnStepThrew(std::string_view obj, std::string_view step,
                                 std::string_view what,
                                 int step_ordinal, int tick)
{
    std::ostringstream os;
    os << Pad(step, kColStep) << " "
       << "[THREW] " << what
       << "  #" << Pad2(step_ordinal) << "/#" << Pad2(tick);
    Emit(obj, os.str());
}

void EnvLogObserver::OnAsyncSubmitted(std::string_view obj, std::string_view step,
                                      std::string_view job)
{
    std::ostringstream os;
    os << Pad(step, kColStep) << " "
       << "ASYNC SUBMIT  " << job;
    Emit(obj, os.str());
}

void EnvLogObserver::OnAsyncCompleted(std::string_view obj, std::string_view job,
                                      uniflow::Duration wait,
                                      bool had_error, bool timed_out)
{
    std::ostringstream os;
    os << Pad("", kColStep) << " "
       << "ASYNC DONE    " << job
       << "  wait=" << FmtMs(wait);
    if (timed_out)      os << "  [TIMEOUT]";
    else if (had_error) os << "  [ERROR]";
    Emit(obj, os.str());
}

void EnvLogObserver::OnSlowCpuStep(std::string_view obj, std::string_view step,
                                   uniflow::Duration cpu)
{
    std::ostringstream os;
    os << Pad(step, kColStep) << " "
       << "[SLOW CPU]  held pump for " << FmtMs(cpu);
    Emit(obj, os.str());
}

void EnvLogObserver::OnSlowAsync(std::string_view obj, std::string_view job,
                                 uniflow::Duration wait_so_far)
{
    std::ostringstream os;
    os << Pad("", kColStep) << " "
       << "[SLOW ASYNC]  " << job
       << "  pending=" << FmtMs(wait_so_far);
    Emit(obj, os.str());
}

void EnvLogObserver::OnFlowEnded(std::string_view obj,
                                 uniflow::StepAction terminal_action,
                                 int final_step_ordinal, int total_ticks,
                                 const std::vector<uniflow::TraceEntry>&,
                                 uniflow::Duration wall_clock,
                                 uniflow::Duration total_cpu,
                                 uniflow::Duration total_async_wait,
                                 const uniflow::FlowStats&,
                                 uniflow::FlowOrigin origin)
{
    std::ostringstream os;
    // `wall`  = flow's start-to-end wall clock.
    // `cpu`   = summed pump-thread time across every step body call.
    // `async` = summed time the flow spent gated on async jobs.
    os << "FLOW "
       << (terminal_action == uniflow::StepAction::Done ? "END  DONE" : "END  FAIL")
       << "  steps=#" << Pad2(final_step_ordinal)
       << " ticks=#" << Pad2(total_ticks)
       << "  wall=" << FmtMs(wall_clock)
       << "  cpu=" << FmtMs(total_cpu)
       << "  async=" << FmtMs(total_async_wait);
    if (origin.file)
        os << "  caller=" << origin.file << ":" << origin.line;
    Emit(obj, os.str());
}

void EnvLogObserver::Emit(std::string_view obj, const std::string& body)
{
    std::lock_guard<std::mutex> lk(mu_);
    std::ostringstream os;
    os << "[" << Pad(obj, kColObj) << "] " << body;
    std::string line = os.str();

    std::cout << line << "\n";
    std::cout.flush();

    std::ofstream fout(path_, std::ios::app);
    if (fout)
        fout << line << "\n";
}
