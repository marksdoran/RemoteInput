#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "ExtLibs/asio/include/asio.hpp"

#include "RemoteInputAccessPolicy.h"
#include "RemoteInputHostResolver.h"
#include "RemoteInputLog.h"
#include "RemoteInputQueue.h"
#include "RemoteInputRateLimiter.h"
#include "RemoteInputSettings.h"

class RemoteInputSession;

// Accepts connections on one or more local addresses, checks each client against the access policy and the
// connection limits, and starts a session for every client it lets in. A local network client whose address is not
// allowed may be an allowed host name at a new address, so when the allowed client list has host names, such a
// client is held until the names have been looked up again.
//
// Open() runs before the network thread starts. Everything else, including every completion handler, runs on the
// network thread, so none of the members need a lock.
class RemoteInputListener final
{
public:
    // Client ids start at `first_client_id`, so that they stay unique when the server is stopped and started again.
    RemoteInputListener(asio::io_context& io_context, RemoteInputAccessPolicy policy,
        const RemoteInputLimits& limits, RemoteInputClientId first_client_id, RemoteInputQueue& queue,
        RemoteInputLog& log);

    RemoteInputListener(const RemoteInputListener&) = delete;
    RemoteInputListener& operator=(const RemoteInputListener&) = delete;

    // Opens a listening socket on `port` at each address. With port 0 the OS picks a free port for the first address
    // that opens, and the same port is used for the others. An address family the computer does not have is
    // skipped; any other failure, such as the port being in use, closes everything and returns false, rather than
    // leaving some clients unable to connect.
    bool Open(const std::vector<asio::ip::address>& addresses, std::uint16_t port);

    // Starts accepting connections on every socket that Open() opened.
    void StartAccepting();

    // Stops accepting and disconnects every client. Once the resulting completions have been handled, the network
    // thread has nothing left to do and its io_context::run() returns.
    void Close();

    // The port being listened on, and a description of where, e.g. "127.0.0.1:4242, [::1]:4242". Both are fixed by
    // Open().
    std::uint16_t Port() const { return port_; }
    const std::string& Endpoints() const { return endpoints_; }

    // The id the next client would get. Read it after the network thread has stopped.
    RemoteInputClientId NextClientId() const
    {
        return next_client_id_;
    }

    // How many messages about clients were left out of the log to keep it readable, since this was last called.
    // Call it after the network thread has stopped, so that the count includes the disconnections Close() caused.
    std::size_t TakeUnloggedMessageCount();

private:
    // A listening socket, plus the timer that pauses accepting on it after an unexpected error.
    struct Acceptor
    {
        explicit Acceptor(asio::io_context& io_context);

        asio::ip::tcp::acceptor socket;
        asio::steady_timer retry_timer;
    };

    // A connected client that is waiting for the host names to be looked up before it is let in or refused.
    struct WaitingClient
    {
        asio::ip::tcp::socket socket;
        asio::ip::address address;
        std::string name;
    };

    void AcceptNext(Acceptor& acceptor);
    void OnAccept(Acceptor& acceptor, const asio::error_code& error, asio::ip::tcp::socket socket);
    void AdmitClient(asio::ip::tcp::socket socket);
    void OnHostNamesLookedUp(const std::vector<asio::ip::address>& addresses);
    void StartSession(asio::ip::tcp::socket socket, const std::string& client_name);
    void LogConnectionMessage(RemoteInputLogLevel level, const std::string& message);
    void OnSessionEnded(RemoteInputClientId client_id);

    asio::io_context& io_context_;
    const RemoteInputLimits limits_;
    RemoteInputQueue& queue_;
    RemoteInputLog& log_;
    RemoteInputAccessPolicy policy_;            // learns new addresses whenever the host names are looked up
    RemoteInputHostResolver host_resolver_;     // declared after policy_, which provides its host names
    RemoteInputClientLogThrottles log_throttles_;
    RemoteInputRateLimiter new_connection_limiter_;   // which new clients get a burst of commands straight away

    std::vector<std::unique_ptr<Acceptor>> acceptors_;   // heap-allocated: handlers keep references to them
    std::vector<WaitingClient> clients_waiting_for_lookup_;
    std::unordered_map<RemoteInputClientId, std::shared_ptr<RemoteInputSession>> sessions_;
    RemoteInputClientId next_client_id_;
    std::uint16_t port_ = 0;
    std::string endpoints_;
    bool is_closing_ = false;
};
