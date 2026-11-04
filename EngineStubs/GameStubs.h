#pragma once

// Stand-ins for the game functions and types that the game-side glue (Game/RemoteInputGame.cpp) uses, so that
// the standalone projects can build and test it. This folder is on the include path of the standalone projects
// only; in the game, the real declarations come from the game's own headers.

#include <string>
#include <vector>

// The game's type for an input callback. The glue only looks these pointers up and passes them on, so the exact
// signature does not matter here.
using InputFunctionType = void (*)(unsigned long value, int state, void* button);

// Finds the callback with the given name, or returns nullptr if there is none. This stand-in knows only a short
// list of the game's callback names (see GameStubs.cpp).
InputFunctionType FindFunctionFromString(const char* str);

// The game's printf-style debug log. This stand-in prints to the console, each line stamped with the seconds
// since the program started.
#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
void MonoPrint(const char* format, ...);

// Test support, stand-in only. After StartRecordingMonoPrint(), MonoPrint() also keeps each line it prints, and
// TakeRecordedMonoPrint() returns and forgets them. Recording is off by default, so a long-running app does not
// keep every line.
void StartRecordingMonoPrint();
std::vector<std::string> TakeRecordedMonoPrint();
