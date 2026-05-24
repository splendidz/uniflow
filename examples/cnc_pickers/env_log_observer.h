// ======================================================================
//  env_log_observer.h - mirrors flow events to console + a log file in
//  fixed-column layout that scans well on a wide monitor.
//
//  Column widths are tuned for ~120 chars terminal. Adjust kColXxx below
//  if the project's typical screen is narrower or you want more
//  description.
//
//  Column layout (STEP rows):
//    [ obj          ] step                       description                   #s/#t elapsed
//    [ Stage        ] OnProcess_WaitHwReady      hw ready handshake settling   #04/#23 elapsed=0.02ms
//
//  The file is opened/closed on every emission. Slow, but never loses a
//  line on a crash. Time cost was explicitly OK for this sim.
//
//  Definitions live in env_log_observer.cpp.
// ======================================================================
#pragma once

#include "uniflow.hpp"

#include <mutex>
#include <string>
#include <string_view>
#include <vector>

class EnvLogObserver : public uniflow::IUniflowObserver
{
public:
    static constexpr int kColObj  = 14;
    static constexpr int kColStep = 28;
    static constexpr int kColDesc = 40;

    explicit EnvLogObserver(std::string file_path);

    void OnFlowStarted(std::string_view obj,
                       uniflow::FlowOrigin origin) override;
    void OnStepRan(std::string_view obj, std::string_view step,
                   std::string_view description,
                   int step_ordinal, int tick,
                   uniflow::Duration elapsed_cpu) override;
    void OnStepThrew(std::string_view obj, std::string_view step,
                     std::string_view what,
                     int step_ordinal, int tick) override;
    void OnAsyncSubmitted(std::string_view obj, std::string_view step,
                          std::string_view job) override;
    void OnAsyncCompleted(std::string_view obj, std::string_view job,
                          uniflow::Duration wait,
                          bool had_error, bool timed_out) override;
    void OnSlowCpuStep(std::string_view obj, std::string_view step,
                       uniflow::Duration cpu) override;
    void OnSlowAsync(std::string_view obj, std::string_view job,
                     uniflow::Duration wait_so_far) override;
    void OnFlowEnded(std::string_view obj,
                     uniflow::StepAction terminal_action,
                     int final_step_ordinal, int total_ticks,
                     const std::vector<uniflow::TraceEntry>&,
                     uniflow::Duration wall_clock,
                     uniflow::Duration total_cpu,
                     uniflow::Duration total_async_wait,
                     const uniflow::FlowStats&,
                     uniflow::FlowOrigin origin) override;

private:
    void Emit(std::string_view obj, const std::string& body);

    std::mutex  mu_;
    std::string path_;
};
