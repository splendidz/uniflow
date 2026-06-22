#include "uf_receiver.h"

#include "mailbox.h"

#include <sstream>

using namespace uniflow;

const char* ToString(RecvState s)
{
    switch (s)
    {
    case RecvState::Idle:        return "Idle";
    case RecvState::Dispatching: return "Dispatching";
    case RecvState::Adding:      return "Adding";
    case RecvState::Subtracting: return "Subtracting";
    }
    return "?";
}

Flow_Receiver::Flow_Receiver(uniflow::Runtime& rt)
    : uniflow::Uniflow<Flow_Receiver>(rt, "Flow_Receiver")
{
    AddTask(ctx_drain_);
}

StepResult Flow_Receiver::Task_Drain::Step1_TakeNext()
{
    if (GlobalEnv::Stop())
    {
        return Done();
    }
    flow().state_ = RecvState::Dispatching;

    // Pop one job. The mailbox is touched only on this pump thread, so no lock.
    Msg m;
    if (!Mailbox::TryPop(m))
    {
        // Queue drained: park the module. Done() lets it idle until the sender
        // relaunches this task (ctx.StartFlow()) on the next burst.
        flow().state_ = RecvState::Idle;
        Describe("queue drained -> done");
        return Done();
    }
    flow().current_ = m;
    Describe("popped ", m.a, ' ', m.op, ' ', m.b);

    // Dispatch by operator: Next() routes to a sibling step in this same task.
    if (m.op == '+')
    {
        return Next(UF_FN(Step2_Add));
    }
    return Next(UF_FN(Step3_Sub));
}

StepResult Flow_Receiver::Task_Drain::Step2_Add()
{
    flow().state_ = RecvState::Adding;
    int               result = flow().current_.a + flow().current_.b;
    std::ostringstream os;
    os << flow().current_.a << " + " << flow().current_.b << " = " << result;
    flow().last_result_ = os.str();
    ++flow().processed_;
    Describe("add: ", flow().last_result_);

    // Loop back to drain the next job.
    return Next(UF_FN(Step1_TakeNext));
}

StepResult Flow_Receiver::Task_Drain::Step3_Sub()
{
    flow().state_ = RecvState::Subtracting;
    int               result = flow().current_.a - flow().current_.b;
    std::ostringstream os;
    os << flow().current_.a << " - " << flow().current_.b << " = " << result;
    flow().last_result_ = os.str();
    ++flow().processed_;
    Describe("sub: ", flow().last_result_);

    return Next(UF_FN(Step1_TakeNext));
}
