#pragma once

// Stand-in for the game's FALCLIB/INCLUDE/ehandler.h, so that the RemoteInput sources build unchanged
// outside the game. Only INT3_ASSERT is provided. This folder is on the include path of the standalone
// projects only; inside the game the real header is used.
//
// The game's INT3_ASSERT breaks into its crash handler. This version reports the failed condition and
// aborts, which a debugger also stops on.

#include <cstdio>
#include <cstdlib>

#define INT3_ASSERT(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            std::fprintf(stderr, "INT3_ASSERT failed: %s (%s:%d)\n", #condition, __FILE__, __LINE__); \
            std::abort(); \
        } \
    } \
    while (false)
