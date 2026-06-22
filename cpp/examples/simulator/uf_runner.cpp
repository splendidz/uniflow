#include "uf_runner.h"

using namespace uniflow;

Flow_Runner::Flow_Runner(uniflow::Runtime& rt, const char* name, int id, double move_ms)
    : uniflow::Uniflow<Flow_Runner>(rt, name),
      clock_(rt.clock()),   // bind to the Runtime's logical clock (scale/freeze)
      id_(id),
      name_(name),
      move_ms_(move_ms)
{
    AddTask(ctx_run_);
    sim::g_rows[id_].name = name_;
}

// Re-anchor virtual time whenever the task is entered.
void Flow_Runner::Task_Run::OnEnter()
{
    phase_start_ = flow().clock_.Now();
}

// Virtual milliseconds elapsed in the current phase. Because Now() comes from
// the VirtualClock, this value stalls when the sim is paused and stretches or
// compresses when it is scaled - the whole point of this example.
double Flow_Runner::Task_Run::VElapsedMs() const
{
    return to_ms(flow().clock_.Now() - phase_start_);
}

void Flow_Runner::Task_Run::Publish(double percent, const char* step)
{
    // Plain writes to our own row - same pump thread as the renderer, no lock.
    sim::RunnerRow& row = sim::g_rows[flow().id_];
    row.percent = percent;
    row.step    = step;
    row.lap     = flow().lap_;
}

StepResult Flow_Runner::Task_Run::Step1_Gate()
{
    if (sim::g_stop.load())   // cooperative shutdown so WaitUntilIdle() can return
    {
        return Done();
    }
    Publish(0.0, "Step1_Gate");
    if (VElapsedMs() < to_ms(Flow_Runner::kGate))
    {
        return Stay();   // re-poll this step next round
    }
    phase_start_ = flow().clock_.Now();   // reset anchor for the Move phase
    Describe("leaving the gate");
    return Next(UF_FN(Step2_Move));
}

StepResult Flow_Runner::Task_Run::Step2_Move()
{
    if (sim::g_stop.load())
    {
        return Done();
    }
    double frac = VElapsedMs() / flow().move_ms_;
    if (frac >= 1.0)
    {
        Publish(100.0, "Step2_Move");
        phase_start_ = flow().clock_.Now();
        Describe("reached the line");
        return Next(UF_FN(Step3_Rest));
    }
    Publish(frac * 100.0, "Step2_Move");
    return Stay();
}

StepResult Flow_Runner::Task_Run::Step3_Rest()
{
    if (sim::g_stop.load())
    {
        return Done();
    }
    Publish(100.0, "Step3_Rest");
    if (VElapsedMs() < to_ms(Flow_Runner::kRest))
    {
        return Stay();
    }
    ++flow().lap_;                        // one full lap completed
    phase_start_ = flow().clock_.Now();
    return Next(UF_FN(Step1_Gate));       // loop forever (until g_stop)
}
