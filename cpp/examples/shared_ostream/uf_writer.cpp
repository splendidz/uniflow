#include "uf_writer.h"

#include "shared_state.h"

using namespace uniflow;

Flow_Writer::Flow_Writer(uniflow::Runtime& rt, std::string text, int count,
                         int turn_id)
    : uniflow::Uniflow<Flow_Writer>(rt, "Flow_Writer"),
      text_(std::move(text)),
      remaining_(count),
      turn_id_(turn_id)
{
    AddTask(task_write_);   // wire the task to this flow (flow() becomes valid)
}

// Step 1: announce the work, then advance to the append loop. Next() stays
// within this task and re-enters at Step2_Loop on the next pump round.
Flow_Writer::StepResult Flow_Writer::Task_Write::Step1_Begin()
{
    Describe("begin: will append \"", flow().text_, "\" x ", flow().remaining_);
    return Next(UF_FN(Step2_Loop));
}

// Step 2: the lock-free core. Both writers run this on the SAME pump thread, so
// touching the shared ostringstream and the shared turn flag needs no mutex.
Flow_Writer::StepResult Flow_Writer::Task_Write::Step2_Loop()
{
    if (flow().remaining_ <= 0)
    {
        Describe("all writes done");
        return Done();   // task finished; the module goes idle
    }
    if (SharedState::Turn() != flow().turn_id_)
    {
        Describe("waiting for turn");
        return Stay();   // not our turn yet - poll again next round
    }

    SharedState::Log() << flow().text_;   // shared sink, no lock
    SharedState::FlipTurn();              // hand the turn to the peer
    --flow().remaining_;
    Describe("appended \"", flow().text_, "\", remaining=", flow().remaining_);
    return Stay();
}
