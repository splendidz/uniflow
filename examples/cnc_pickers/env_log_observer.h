// env_log_observer.h - fixed-column logger to stdout + appended log file.
// File is opened/closed per line so a crash never loses output.
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
    void OnStepChanged(std::string_view obj, std::string_view step,
                       std::string_view description,
                       int step_ordinal, int ticks_in_step,
                       double elapsed_ms) override;
    void OnStepThrew(std::string_view obj, std::string_view step,
                     std::string_view what,
                     int step_ordinal, int tick) override;
    void OnAsyncSubmitted(std::string_view obj, std::string_view step,
                          std::string_view job) override;
    void OnAsyncCompleted(std::string_view obj, std::string_view job,
                          double wait_ms,
                          bool had_error, bool timed_out) override;
    void OnSlowCpuStep(std::string_view obj, std::string_view step,
                       double cpu_ms) override;
    void OnSlowAsync(std::string_view obj, std::string_view job,
                     double wait_so_far_ms) override;
    void OnFlowEnded(std::string_view obj,
                     uniflow::StepAction terminal_action,
                     int final_step_ordinal, int total_ticks,
                     const std::vector<uniflow::TraceEntry>&,
                     double wall_ms,
                     double total_step_ms,
                     double total_async_ms,
                     const uniflow::FlowStats&,
                     uniflow::FlowOrigin origin) override;

private:
    void Emit(std::string_view obj, const std::string& body);

    std::mutex  mu_;
    std::string path_;
};
