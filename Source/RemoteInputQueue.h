#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "RemoteInputLineProtocol.h"

// Identifies one client connection for the lifetime of the server. Ids are never reused.
using RemoteInputClientId = std::uint32_t;

// A command received from a remote client, ready for the game to act on.
struct RemoteInputCommand
{
    RemoteInputClientId client_id = 0;
    std::string callback;                                   // callback name without any suffix, e.g. "SimEject"
    RemoteInputAction action = RemoteInputAction::PressAndRelease;
    std::chrono::steady_clock::time_point received_at;      // lets the game ignore presses that went stale, e.g.
                                                            // ones that arrived while no mission was running;
                                                            // releases must never be ignored
};

// Hands commands from the network thread to the game. Any number of threads may push; one thread drains. The
// lock is only ever held long enough to append one command or to swap two vectors, so draining never waits for
// network activity.
class RemoteInputQueue final
{
public:
    explicit RemoteInputQueue(std::size_t max_queued_commands);

    RemoteInputQueue(const RemoteInputQueue&) = delete;
    RemoteInputQueue& operator=(const RemoteInputQueue&) = delete;

    // Adds a command, unless max_queued_commands commands are already waiting to be drained, in which case the
    // command is dropped and false is returned.
    bool TryPush(RemoteInputClientId client_id, std::string callback, RemoteInputAction action);

    // Adds a command even if the queue is full. Only for releasing an input that the game has already been told
    // to press: losing that release would leave the input held down. Callers bound how many of these they push.
    void PushEvenIfFull(RemoteInputClientId client_id, std::string callback, RemoteInputAction action);

    // Replaces the contents of `commands` with everything pushed since the last call, oldest first. The vectors
    // are swapped rather than copied, so passing the same vector every frame does not allocate once both have
    // grown to the usual size.
    void DrainInto(std::vector<RemoteInputCommand>& commands);

private:
    const std::size_t max_queued_commands_;

    std::mutex mutex_;
    std::vector<RemoteInputCommand> pending_;   // guarded by mutex_
};
