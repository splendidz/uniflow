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
    std::string FmtMs(double ms)
    {
        std::ostringstream os;
        os.setf(std::ios::fixed);
        os.precision(2);
        os << ms << "ms";
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
    {
        os << "  caller=" << origin.file << ":" << origin.line;
        if (origin.function)
            os << " " << origin.function << "()";
    }
    Emit(obj, os.str());
}

void EnvLogObserver::OnStepChanged(std::string_view obj,
                                   std::string_view prev_step,
                                   std::string_view next_step,
                                   std::string_view description,
                                   int step_ordinal,
                                   double elapsed_ms,
                                   const uniflow::TickStats& step_ticks)
{
    std::string transition(prev_step);
    if (!next_step.empty())
    {
        transition += " -> ";
        transition += std::string(next_step);
    }
    std::ostringstream os;
    os << Pad(transition, kColStep) << " "
       << Pad(description, kColDesc) << " "
       << "#" << Pad2(step_ordinal)
       << " elapsed=" << FmtMs(elapsed_ms)
       << "  tick x" << step_ticks.count
       << " avg=" << FmtMs(step_ticks.avg_ms)
       << " min=" << FmtMs(step_ticks.min_ms)
       << " max=" << FmtMs(step_ticks.max_ms);
    Emit(obj, os.str());
}

void EnvLogObserver::OnStepThrew(std::string_view obj, std::string_view step,
                                 std::string_view what,
                                 int step_ordinal, int /*tick*/)
{
    std::ostringstream os;
    os << Pad(step, kColStep) << " "
       << "[THREW] " << what
       << "  #" << Pad2(step_ordinal);
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
                                      double wait_ms,
                                      bool had_error, bool timed_out)
{
    std::ostringstream os;
    os << Pad("", kColStep) << " "
       << "ASYNC DONE    " << job
       << "  wait=" << FmtMs(wait_ms);
    if (timed_out)      os << "  [TIMEOUT]";
    else if (had_error) os << "  [ERROR]";
    Emit(obj, os.str());
}

void EnvLogObserver::OnSlowCpuStep(std::string_view obj, std::string_view step,
                                   double cpu_ms)
{
    std::ostringstream os;
    os << Pad(step, kColStep) << " "
       << "[SLOW CPU]  held pump for " << FmtMs(cpu_ms);
    Emit(obj, os.str());
}

void EnvLogObserver::OnSlowAsync(std::string_view obj, std::string_view job,
                                 double wait_so_far_ms)
{
    std::ostringstream os;
    os << Pad("", kColStep) << " "
       << "[SLOW ASYNC]  " << job
       << "  pending=" << FmtMs(wait_so_far_ms);
    Emit(obj, os.str());
}

void EnvLogObserver::OnFlowEnded(std::string_view obj,
                                 uniflow::StepAction terminal_action,
                                 int final_step_ordinal,
                                 const std::vector<uniflow::TraceEntry>&,
                                 double wall_ms,
                                 double total_step_ms,
                                 double total_async_ms,
                                 const uniflow::FlowTickSummary& flow_ticks,
                                 const uniflow::FlowStats&,
                                 uniflow::FlowOrigin origin)
{
    std::ostringstream os;
    os << "FLOW "
       << (terminal_action == uniflow::StepAction::Done ? "END  DONE" : "END  FAIL")
       << "  steps=#" << Pad2(final_step_ordinal)
       << "  wall=" << FmtMs(wall_ms)
       << "  step=" << FmtMs(total_step_ms)
       << "  async=" << FmtMs(total_async_ms)
       << "  tick x" << flow_ticks.all.count
       << " avg=" << FmtMs(flow_ticks.all.avg_ms)
       << " min=" << FmtMs(flow_ticks.all.min_ms)
       << " max=" << FmtMs(flow_ticks.all.max_ms);
    if (!flow_ticks.max_step.empty())
        os << " (" << flow_ticks.max_step << ")";
    if (origin.file)
    {
        os << "  caller=" << origin.file << ":" << origin.line;
        if (origin.function)
            os << " " << origin.function << "()";
    }
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
