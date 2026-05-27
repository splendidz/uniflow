#include "uf_writer.h"

#include "shared_state.h"

UF_Writer::StepResult UF_Writer::OnWrite_Begin(std::string text,
                                               int count, int turn_id)
{
    text_      = std::move(text);
    remaining_ = count;
    turn_id_   = turn_id;
    Describe("begin: will append \"", text_, "\" x ", remaining_);
    return UF_NEXT(OnWrite_Loop);
}

UF_Writer::StepResult UF_Writer::OnWrite_Loop()
{
    if (remaining_ <= 0)
    {
        Describe("all writes done");
        return Done();
    }
    if (SharedState::Turn() != turn_id_)
    {
        Describe("waiting for turn");
        return Stay();
    }

    SharedState::Log() << text_;
    SharedState::FlipTurn();
    --remaining_;
    Describe("appended \"", text_, "\", remaining=", remaining_);
    return Stay();
}
