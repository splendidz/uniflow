// shared_state.h - one shared ostringstream and a single turn flag are
// touched by every UF_Writer running on the same Runtime. Because the
// Runtime runs all steps on ONE pump thread, no lock is needed even
// though the writers look "parallel" from the outside.
#pragma once

#include <sstream>

class SharedState
{
public:
    // The single sink every writer appends to.
    static std::ostringstream& Log();

    // Whose turn is it to write? 0 = first writer, 1 = second writer.
    // Compared and flipped from inside step bodies on the pump thread.
    static int  Turn();
    static void FlipTurn();

    SharedState() = delete;
};
