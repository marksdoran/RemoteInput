#pragma once

#include <vector>

#include "GameStubs.h"   // in the game: the headers that declare InputFunctionType and FindFunctionFromString()
#include "RemoteInputQueue.h"

// The game's side of remote input. These three functions are all the rest of the game needs: the WinMain startup
// and shutdown paths start and stop the server, and the input loop collects what remote clients asked for once per
// frame, next to the keyboard, mouse, joystick and XInput handling.

// A callback for the input loop to invoke on behalf of a remote client.
struct RemoteInputCall
{
    InputFunctionType function = nullptr;
    RemoteInputCommand command;   // the callback's name, whether to press, release or both, and who asked
};

// Starts listening if the config variables allow it. Returns true if the server is now accepting connections;
// otherwise the log says why not. Call from the WinMain startup path.
bool StartRemoteInput();

// Disconnects every client and stops listening. Call from the WinMain shutdown path, on the thread that called
// StartRemoteInput().
void StopRemoteInput();

// Replaces the contents of `calls` with the callbacks that remote clients have asked for since the previous call,
// oldest first. Commands naming a callback that does not exist are skipped, and each such name is logged once.
// Call once per frame from the input loop; it never waits for the network, and it is safe to call whether or not
// remote input is running.
//
// Every release must be invoked, even when the input loop chooses to skip presses that waited too long (see
// RemoteInputCommand::received_at): a release may be the only thing that lets go of an input the game is holding.
void GetRemoteInput(std::vector<RemoteInputCall>& calls);
