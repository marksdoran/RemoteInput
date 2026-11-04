#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <string_view>

#include "ExtLibs/asio/include/asio.hpp"

#include "RemoteInputQueue.h"
#include "RemoteInputRateLimiter.h"
#include "RemoteInputSettings.h"

class RemoteInputLog;

// One connected client. Reads lines from the client and pushes its commands to the queue until the client
// disconnects, sends a line that is too long, or the server closes the connection. When the connection ends for any
// of those reasons, anything the client pressed but did not release is released for it. A client that sends commands
// faster than it may is slowed down: the session stops reading until its allowance has built up again.
//
// All member functions run on the network thread. Every pending read holds a shared_ptr to its session, so a session
// lives until its last read has completed, whether or not the listener still refers to it.
class RemoteInputSession final : public std::enable_shared_from_this<RemoteInputSession>
{
public:
    // Called exactly once, when the connection has ended for whatever reason.
    using EndedHandler = std::function<void(RemoteInputClientId)>;

    // `command_allowance` says whether the client may send a burst of commands straight away, or has to wait for
    // its allowance to build up. `log_throttles` belongs to the listener, which outlives every session.
    RemoteInputSession(asio::ip::tcp::socket socket, RemoteInputClientId client_id, std::string_view client_name,
        const RemoteInputLimits& limits, RemoteInputInitialAllowance command_allowance, RemoteInputQueue& queue,
        RemoteInputLog& log, RemoteInputClientLogThrottles& log_throttles, EndedHandler on_ended);

    RemoteInputSession(const RemoteInputSession&) = delete;
    RemoteInputSession& operator=(const RemoteInputSession&) = delete;

    // Starts reading. Call once, after construction.
    void Start();

    // Closes the connection. The pending read or pause then ends with an error, which ends the session.
    void Close();

private:
    void ReadNextLine();
    void OnLineRead(const asio::error_code& error, std::size_t line_length);
    void OnLineAvailable(std::size_t line_length);
    void PauseUntilAllowed(std::size_t line_length);

    // Each returns false, having done nothing, if the client has to wait for its allowance before this line.
    bool HandleLine(std::string_view line);
    bool HandleCommand(std::string_view command);

    void WarnAboutLine(std::string_view problem);
    void End(const asio::error_code& error);
    void ReleaseHeldInputs();

    asio::ip::tcp::socket socket_;
    asio::steady_timer pace_timer_;       // waits for the allowance while the client is being slowed down
    const RemoteInputClientId client_id_;
    const std::string log_prefix_;        // e.g. "Client 3 (192.168.1.20:53122)"
    const std::size_t max_line_length_;
    const std::size_t max_held_inputs_;
    const double max_commands_per_second_;
    RemoteInputQueue& queue_;
    RemoteInputLog& log_;
    RemoteInputClientLogThrottles& log_throttles_;
    EndedHandler on_ended_;
    RemoteInputRateLimiter command_limiter_;
    std::string received_;                // bytes received but not yet handled as complete lines
    // Callbacks the client has pressed and not yet released. std::less<> allows lookups by string_view.
    std::set<std::string, std::less<>> held_inputs_;
    bool is_first_line_ = true;
    bool has_reported_slowing_down_;      // so that it is logged once per connection at most
    bool closed_by_server_ = false;
};
