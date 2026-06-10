#include "uf_receiver.h"

#include "mailbox.h"

#include <iostream>
#include <sstream>

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

UF_Receiver::StepResult UF_Receiver::OnRecv_Begin()
{
    Describe("woken by sender");
    return UF_NEXT(OnRecv_TakeNext);
}

UF_Receiver::StepResult UF_Receiver::OnRecv_TakeNext()
{
    state_ = RecvState::Dispatching;
    Msg m;
    if (!Mailbox::TryPop(m))
    {
        state_ = RecvState::Idle;
        Describe("queue drained -> done");
        return Done();
    }
    current_ = m;
    Describe("popped ", m.a, ' ', m.op, ' ', m.b);
    if (m.op == '+') return UF_NEXT(OnRecv_Add);
    return UF_NEXT(OnRecv_Sub);
}

UF_Receiver::StepResult UF_Receiver::OnRecv_Add()
{
    state_ = RecvState::Adding;
    int result = current_.a + current_.b;
    std::ostringstream os;
    os << current_.a << " + " << current_.b << " = " << result;
    last_result_ = os.str();
    ++processed_;
    std::cout << "[receiver] add: " << last_result_
              << "   (processed=" << processed_
              << " queue=" << Mailbox::Size() << ")\n";
    Describe("add: ", last_result_);
    return UF_NEXT(OnRecv_TakeNext);
}

UF_Receiver::StepResult UF_Receiver::OnRecv_Sub()
{
    state_ = RecvState::Subtracting;
    int result = current_.a - current_.b;
    std::ostringstream os;
    os << current_.a << " - " << current_.b << " = " << result;
    last_result_ = os.str();
    ++processed_;
    std::cout << "[receiver] sub: " << last_result_
              << "   (processed=" << processed_
              << " queue=" << Mailbox::Size() << ")\n";
    Describe("sub: ", last_result_);
    return UF_NEXT(OnRecv_TakeNext);
}
