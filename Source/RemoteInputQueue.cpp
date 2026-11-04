// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: http://www.viva64.com
#include "RemoteInputQueue.h"

#include <utility>

namespace
{
RemoteInputCommand MakeCommand(RemoteInputClientId client_id, std::string callback, RemoteInputAction action)
{
    RemoteInputCommand command;
    command.client_id = client_id;
    command.callback = std::move(callback);
    command.action = action;
    command.received_at = std::chrono::steady_clock::now();
    return command;
}
} // namespace

RemoteInputQueue::RemoteInputQueue(std::size_t max_queued_commands) :
    max_queued_commands_(max_queued_commands)
{
}

bool RemoteInputQueue::TryPush(RemoteInputClientId client_id, std::string callback, RemoteInputAction action)
{
    RemoteInputCommand command = MakeCommand(client_id, std::move(callback), action);

    // The size check and the push share one lock, so concurrent producers cannot overfill the queue.
    const std::lock_guard lock(mutex_);
    if (pending_.size() >= max_queued_commands_)
    {
        return false;
    }
    pending_.push_back(std::move(command));
    return true;
}

void RemoteInputQueue::PushEvenIfFull(RemoteInputClientId client_id, std::string callback, RemoteInputAction action)
{
    RemoteInputCommand command = MakeCommand(client_id, std::move(callback), action);

    const std::lock_guard lock(mutex_);
    pending_.push_back(std::move(command));
}

void RemoteInputQueue::DrainInto(std::vector<RemoteInputCommand>& commands)
{
    // Destroy the previous frame's commands before taking the lock, so the swap is all that happens under it.
    commands.clear();

    const std::lock_guard lock(mutex_);
    pending_.swap(commands);
}
