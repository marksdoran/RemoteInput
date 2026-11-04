// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: http://www.viva64.com
#include "RemoteInputSession.h"

#include <format>
#include <utility>

#include "RemoteInputLineProtocol.h"
#include "RemoteInputLog.h"

RemoteInputSession::RemoteInputSession(asio::ip::tcp::socket socket, RemoteInputClientId client_id,
    std::string_view client_name, const RemoteInputLimits& limits, RemoteInputInitialAllowance command_allowance,
    RemoteInputQueue& queue, RemoteInputLog& log, RemoteInputClientLogThrottles& log_throttles,
    EndedHandler on_ended) :
    socket_(std::move(socket)),
    pace_timer_(socket_.get_executor()),
    client_id_(client_id),
    log_prefix_(std::format("Client {} ({})", client_id, client_name)),
    max_line_length_(limits.max_line_length),
    max_held_inputs_(limits.max_held_inputs),
    max_commands_per_second_(limits.max_commands_per_second),
    queue_(queue),
    log_(log),
    log_throttles_(log_throttles),
    on_ended_(std::move(on_ended)),
    command_limiter_(limits.max_commands_per_second, limits.max_commands_per_second, command_allowance),
    // A client that starts with no allowance has already been told, when it connected, that it will be slowed down.
    has_reported_slowing_down_(command_allowance == RemoteInputInitialAllowance::Empty)
{
}

void RemoteInputSession::Start()
{
    ReadNextLine();
}

void RemoteInputSession::Close()
{
    // Either a read or a pause is under way, never both; whichever it is ends with an error, which ends the session.
    closed_by_server_ = true;
    asio::error_code ignored;
    socket_.close(ignored);
    pace_timer_.cancel();
}

void RemoteInputSession::ReadNextLine()
{
    // Capping the buffer at one line plus its LF is what bounds memory use: once the buffer is full without a LF in
    // it, the read fails with asio::error::not_found.
    asio::async_read_until(socket_, asio::dynamic_buffer(received_, max_line_length_ + 1), '\n',
        [self = shared_from_this()](const asio::error_code& error, std::size_t line_length)
        {
            self->OnLineRead(error, line_length);
        });
}

void RemoteInputSession::OnLineRead(const asio::error_code& error, std::size_t line_length)
{
    // A read can finish just before Close(), with its handler still to run; nothing more is handled after Close().
    if (error || closed_by_server_)
    {
        End(error);
        return;
    }
    OnLineAvailable(line_length);
}

void RemoteInputSession::OnLineAvailable(std::size_t line_length)
{
    // line_length includes the LF, which is not part of the line.
    if (!HandleLine(std::string_view(received_).substr(0, line_length - 1)))
    {
        PauseUntilAllowed(line_length);
        return;
    }
    received_.erase(0, line_length);

    // Not recursion: ASIO never calls a completion handler from inside the call that started the operation, so each
    // read completes on a fresh stack.
    ReadNextLine();
}

void RemoteInputSession::PauseUntilAllowed(std::size_t line_length)
{
    // The line stays in received_, and nothing more is read until it has been handled, so TCP holds back whatever
    // else the client sends: a client sending too fast is slowed down rather than having its commands dropped.
    if (!has_reported_slowing_down_ && log_throttles_.lines.Allow())
    {
        WarnAboutLine(std::format("slowing its commands down to {} a second", max_commands_per_second_));
    }
    has_reported_slowing_down_ = true;

    pace_timer_.expires_after(command_limiter_.TimeUntilAvailable());
    pace_timer_.async_wait([self = shared_from_this(), line_length](const asio::error_code& error)
        {
            // Close() cancels the wait. It may also come just after the wait has finished, so check for it as well.
            if (error || self->closed_by_server_)
            {
                self->End(error);
                return;
            }
            self->OnLineAvailable(line_length);
        });
}

bool RemoteInputSession::HandleLine(std::string_view line)
{
    const RemoteInputLine parsed = ParseRemoteInputLine(is_first_line_ ? RemoveByteOrderMark(line) : line);
    switch (parsed.kind)
    {
    case RemoteInputLineKind::Empty:
        break;
    case RemoteInputLineKind::Invalid:
        if (log_throttles_.lines.Allow())
        {
            WarnAboutLine(std::format("ignored {}: callback names can only contain A-Z, a-z, 0-9 and _",
                QuoteForRemoteInputLog(parsed.text)));
        }
        break;
    case RemoteInputLineKind::Command:
        if (!HandleCommand(parsed.text))
        {
            return false;
        }
        break;
    }
    is_first_line_ = false;
    return true;
}

bool RemoteInputSession::HandleCommand(std::string_view command)
{
    // Only name characters get this far, so the command is safe to log as it is.
    const RemoteInputCallbackRequest request = SplitRemoteInputCommand(command);
    const auto held = held_inputs_.find(request.callback);

    if (request.action == RemoteInputAction::Release && held != held_inputs_.end())
    {
        // The game has been told to press this input, so its release must get through even if the client is over its
        // rate or the queue is full. There can be no more of these than presses that got through.
        queue_.PushEvenIfFull(client_id_, std::string(request.callback), request.action);
        held_inputs_.erase(held);
        log_.Write(RemoteInputLogLevel::Verbose, std::format("{}: {}", log_prefix_, command));
        return true;
    }

    if (!command_limiter_.TryAcquire())
    {
        return false;
    }
    const bool is_new_press = request.action == RemoteInputAction::Press && held == held_inputs_.end();
    if (is_new_press && held_inputs_.size() >= max_held_inputs_)
    {
        if (log_throttles_.lines.Allow())
        {
            WarnAboutLine(std::format("dropped {}: each connection can hold at most {} inputs down at once", command,
                max_held_inputs_));
        }
        return true;
    }
    if (!queue_.TryPush(client_id_, std::string(request.callback), request.action))
    {
        if (log_throttles_.lines.Allow())
        {
            WarnAboutLine(std::format("dropped {}: too many commands are waiting for the game", command));
        }
        return true;
    }

    if (is_new_press)
    {
        held_inputs_.emplace(request.callback);
    }
    log_.Write(RemoteInputLogLevel::Verbose, std::format("{}: {}", log_prefix_, command));
    return true;
}

void RemoteInputSession::WarnAboutLine(std::string_view problem)
{
    log_.Write(RemoteInputLogLevel::Warning,
        std::format("{}: {}{}", log_prefix_, problem, log_throttles_.lines.TakeSuppressedNote()));
}

void RemoteInputSession::End(const asio::error_code& error)
{
    if (log_throttles_.connections.Allow())
    {
        std::string message;
        RemoteInputLogLevel level = RemoteInputLogLevel::Info;
        if (closed_by_server_)
        {
            message = "disconnected by the server";
        }
        else if (error == asio::error::eof && received_.empty())
        {
            message = "disconnected";
        }
        else if (error == asio::error::eof)
        {
            // Acting on part of a line could do the wrong thing, e.g. "SimEject" from a cut-off "SimEject_Release".
            level = RemoteInputLogLevel::Warning;
            message = std::format("disconnected, so its unfinished line {} was discarded",
                QuoteForRemoteInputLog(received_));
        }
        else if (error == asio::error::not_found)
        {
            level = RemoteInputLogLevel::Warning;
            message = std::format("disconnected for sending a line longer than {} bytes, starting {}", max_line_length_,
                QuoteForRemoteInputLog(received_));
        }
        else
        {
            message = std::format("connection lost: {}", error.message());
        }
        log_.Write(level,
            std::format("{}: {}{}", log_prefix_, message, log_throttles_.connections.TakeSuppressedNote()));
    }

    asio::error_code ignored;
    socket_.close(ignored);
    ReleaseHeldInputs();
    on_ended_(client_id_);
}

void RemoteInputSession::ReleaseHeldInputs()
{
    if (!held_inputs_.empty() && log_throttles_.connections.Allow())
    {
        std::string callbacks;
        for (const std::string& callback : held_inputs_)
        {
            callbacks += callbacks.empty() ? "" : ", ";
            callbacks += callback;
        }
        log_.Write(RemoteInputLogLevel::Info, std::format("{}: releasing what the client left pressed: {}{}",
            log_prefix_, callbacks, log_throttles_.connections.TakeSuppressedNote()));
    }

    // At most max_held_inputs_ of these, so pushing them past the queue limit stays bounded.
    for (const std::string& callback : held_inputs_)
    {
        queue_.PushEvenIfFull(client_id_, callback, RemoteInputAction::Release);
    }
    held_inputs_.clear();
}
