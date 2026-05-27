#include "shared_state.h"

namespace
{
    std::ostringstream g_log;
    int                g_turn = 0;
}

std::ostringstream& SharedState::Log() { return g_log; }
int                 SharedState::Turn() { return g_turn; }
void                SharedState::FlipTurn() { g_turn = 1 - g_turn; }
