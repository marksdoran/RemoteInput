// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: http://www.viva64.com
#include "RemoteInputListener.h"

#include <chrono>
#include <format>
#include <system_error>
#include <utility>

#include "RemoteInputLog.h"
#include "RemoteInputSession.h"

namespace
{
// How long to pause accepting on a socket after an accept error. ASIO already retries the harmless ones, such as a
// client giving up before its connection was accepted; what reaches the listener is usually resource exhaustion
// (e.g. no socket handles left), which retrying at once would turn into a busy loop that floods the log.
constexpr std::chrono::milliseconds accept_retry_delay{ 500 };

// "192.168.1.20:53122" for IPv4, "[fe80::1]:53122" for IPv6.
std::string FormatEndpoint(const asio::ip::tcp::endpoint& endpoint)
{
    const asio::ip::address address = NormalizeAddress(endpoint.address());
    if (address.is_v6())
    {
        return std::format("[{}]:{}", address.to_string(), endpoint.port());
    }
    return std::format("{}:{}", address.to_string(), endpoint.port());
}

// True for listening errors that only mean the computer lacks this kind of address, e.g. IPv6 or ::1, so that the
// other addresses can still be used.
bool IsMissingAddressFamily(const asio::error_code& error)
{
    return error == asio::error::address_family_not_supported || error == std::errc::address_not_available;
}

// Opens `acceptor` and makes it listen on `endpoint`, stopping at the first step that fails.
asio::error_code Listen(asio::ip::tcp::acceptor& acceptor, const asio::ip::tcp::endpoint& endpoint)
{
    asio::error_code error;
    acceptor.open(endpoint.protocol(), error);
    if (error)
    {
        return error;
    }

    if (endpoint.address().is_v6())
    {
        // IPv4 and IPv6 get separate sockets, so every platform behaves the same way and IPv4 clients never show
        // up as IPv4-mapped IPv6 addresses.
        acceptor.set_option(asio::ip::v6_only(true), error);
        if (error)
        {
            return error;
        }
    }

#if !defined(_WIN32)
    // Lets the server listen again straight after a restart while old connections linger in TIME_WAIT; another
    // program still cannot listen on the port while the server does. Windows needs no option for this. There, a
    // program running as the same user could take the port over by asking for SO_REUSEADDR; SO_EXCLUSIVEADDRUSE
    // would prevent that, but it can also keep the port unusable for minutes after a restart while old connections
    // finish closing, and a program running as the player has easier ways to interfere with the game anyway.
    acceptor.set_option(asio::socket_base::reuse_address(true), error);
    if (error)
    {
        return error;
    }
#endif

    acceptor.bind(endpoint, error);
    if (error)
    {
        return error;
    }
    acceptor.listen(asio::socket_base::max_listen_connections, error);
    return error;
}
} // namespace

RemoteInputListener::Acceptor::Acceptor(asio::io_context& io_context) :
    socket(io_context),
    retry_timer(io_context)
{
}

RemoteInputListener::RemoteInputListener(asio::io_context& io_context, RemoteInputAccessPolicy policy,
    const RemoteInputLimits& limits, RemoteInputClientId first_client_id, RemoteInputQueue& queue,
    RemoteInputLog& log) :
    io_context_(io_context),
    limits_(limits),
    queue_(queue),
    log_(log),
    policy_(std::move(policy)),
    host_resolver_(io_context, policy_.HostNames(), limits.host_lookup_interval, log,
        [this](const std::vector<asio::ip::address>& addresses)
        {
            OnHostNamesLookedUp(addresses);
        }),
    new_connection_limiter_(static_cast<double>(limits.max_connections), limits.max_new_connections_per_second),
    next_client_id_(first_client_id)
{
}

bool RemoteInputListener::Open(const std::vector<asio::ip::address>& addresses, std::uint16_t port)
{
    port_ = port;
    for (const asio::ip::address& address : addresses)
    {
        const asio::ip::tcp::endpoint endpoint(address, port_);
        auto acceptor = std::make_unique<Acceptor>(io_context_);
        const asio::error_code error = Listen(acceptor->socket, endpoint);
        if (IsMissingAddressFamily(error))
        {
            log_.Write(RemoteInputLogLevel::Info, std::format("Not listening on {}: this computer has no such address",
                FormatEndpoint(endpoint)));
            continue;
        }
        if (error)
        {
            // Listening on only some of the addresses could send clients of the others to whatever program holds
            // the port there, so it is all or nothing.
            log_.Write(RemoteInputLogLevel::Error,
                std::format("Cannot listen on {}: {}", FormatEndpoint(endpoint), error.message()));
            acceptors_.clear();
            endpoints_.clear();
            return false;
        }

        asio::error_code ignored;
        const asio::ip::tcp::endpoint bound_endpoint = acceptor->socket.local_endpoint(ignored);
        if (port_ == 0)
        {
            port_ = bound_endpoint.port();   // the OS picked a port: use it for the other addresses too
        }
        endpoints_ += endpoints_.empty() ? "" : ", ";
        endpoints_ += FormatEndpoint(bound_endpoint);
        acceptors_.push_back(std::move(acceptor));
    }
    return !acceptors_.empty();
}

void RemoteInputListener::StartAccepting()
{
    for (const std::unique_ptr<Acceptor>& acceptor : acceptors_)
    {
        AcceptNext(*acceptor);
    }
}

void RemoteInputListener::Close()
{
    is_closing_ = true;
    for (const std::unique_ptr<Acceptor>& acceptor : acceptors_)
    {
        asio::error_code ignored;
        acceptor->socket.close(ignored);
        acceptor->retry_timer.cancel();
    }
    host_resolver_.Close();
    clients_waiting_for_lookup_.clear();   // closes their sockets

    // Each session removes itself from sessions_ when its pending read completes, which happens later, so iterating
    // here is safe.
    for (const auto& [client_id, session] : sessions_)
    {
        session->Close();
    }
}

std::size_t RemoteInputListener::TakeUnloggedMessageCount()
{
    return log_throttles_.connections.TakeSuppressedCount() + log_throttles_.lines.TakeSuppressedCount();
}

void RemoteInputListener::AcceptNext(Acceptor& acceptor)
{
    acceptor.socket.async_accept(
        [this, &acceptor](const asio::error_code& error, asio::ip::tcp::socket socket)
        {
            OnAccept(acceptor, error, std::move(socket));
        });
}

void RemoteInputListener::OnAccept(Acceptor& acceptor, const asio::error_code& error,
    asio::ip::tcp::socket socket)
{
    if (is_closing_)
    {
        return;   // Close() was called; any accepted socket closes as it goes out of scope
    }

    if (error)
    {
        log_.Write(RemoteInputLogLevel::Warning, std::format("Accepting a connection failed: {}", error.message()));
        acceptor.retry_timer.expires_after(accept_retry_delay);
        acceptor.retry_timer.async_wait([this, &acceptor](const asio::error_code& wait_error)
            {
                if (!wait_error && !is_closing_)
                {
                    AcceptNext(acceptor);
                }
            });
        return;
    }

    AdmitClient(std::move(socket));
    AcceptNext(acceptor);
}

void RemoteInputListener::AdmitClient(asio::ip::tcp::socket socket)
{
    // A refused socket is closed as it goes out of scope; the client sees the connection close at once.
    asio::error_code error;
    const asio::ip::tcp::endpoint client_endpoint = socket.remote_endpoint(error);
    if (error)
    {
        LogConnectionMessage(RemoteInputLogLevel::Info,
            std::format("A client disconnected before it could be identified: {}", error.message()));
        return;
    }
    const asio::ip::address client_address = NormalizeAddress(client_endpoint.address());
    std::string client_name = FormatEndpoint(client_endpoint);

    if (policy_.IsAllowed(client_address))
    {
        StartSession(std::move(socket), client_name);
        return;
    }

    // A local network client may be an allowed host name whose address has changed, so look the names up again
    // before deciding. Nobody else could ever be allowed, so nobody else gets to cause a lookup.
    if (policy_.CouldBeAllowedByName(client_address))
    {
        // Clients held like this are capped like sessions.
        if (clients_waiting_for_lookup_.size() >= limits_.max_connections)
        {
            LogConnectionMessage(RemoteInputLogLevel::Warning, std::format(
                "Refused connection from {}: too many clients are waiting for the allowed host names to be looked up",
                client_name));
            return;
        }
        if (host_resolver_.LookUp())
        {
            LogConnectionMessage(RemoteInputLogLevel::Info, std::format(
                "Looking up the allowed host names before deciding whether {} may connect", client_name));
            clients_waiting_for_lookup_.push_back({ std::move(socket), client_address, std::move(client_name) });
            return;
        }
    }
    LogConnectionMessage(RemoteInputLogLevel::Warning,
        std::format("Refused connection from {}: not in the allowed client list", client_name));
}

void RemoteInputListener::OnHostNamesLookedUp(const std::vector<asio::ip::address>& addresses)
{
    policy_.SetHostAddresses(addresses);

    // The sockets of refused clients close when waiting_clients goes out of scope.
    std::vector<WaitingClient> waiting_clients = std::exchange(clients_waiting_for_lookup_, {});
    for (WaitingClient& client : waiting_clients)
    {
        if (policy_.IsAllowed(client.address))
        {
            StartSession(std::move(client.socket), client.name);
        }
        else
        {
            LogConnectionMessage(RemoteInputLogLevel::Warning,
                std::format("Refused connection from {}: not in the allowed client list", client.name));
        }
    }
}

void RemoteInputListener::StartSession(asio::ip::tcp::socket socket, const std::string& client_name)
{
    if (sessions_.size() >= limits_.max_connections)
    {
        LogConnectionMessage(RemoteInputLogLevel::Warning, std::format(
            "Refused connection from {}: too many clients are connected (the limit is {})", client_name,
            limits_.max_connections));
        return;
    }

    // A client that reconnects in a loop must not get a fresh burst of commands each time. Refusing the connections
    // that come too often would let one such client lock every other client out, so they are let in, but their
    // commands wait for the allowance to build up at the usual rate.
    const bool gets_command_burst = new_connection_limiter_.TryAcquire();

    // Lets the OS eventually notice a client that vanished without closing its connection, such as a phone that
    // left the Wi-Fi, so that its connection slot is freed. Not essential, so a failure is ignored.
    asio::error_code ignored;
    socket.set_option(asio::socket_base::keep_alive(true), ignored);

    const RemoteInputClientId client_id = next_client_id_++;
    auto session = std::make_shared<RemoteInputSession>(std::move(socket), client_id, client_name, limits_,
        gets_command_burst ? RemoteInputInitialAllowance::FullBurst : RemoteInputInitialAllowance::Empty, queue_,
        log_, log_throttles_,
        [this](RemoteInputClientId ended_client_id)
        {
            OnSessionEnded(ended_client_id);
        });
    sessions_.emplace(client_id, session);

    std::string message = std::format("Client {} connected from {}", client_id, client_name);
    if (!gets_command_burst)
    {
        message += "; clients are connecting very often, so its commands will be slowed down at first";
    }
    LogConnectionMessage(RemoteInputLogLevel::Info, message);
    session->Start();
}

void RemoteInputListener::LogConnectionMessage(RemoteInputLogLevel level, const std::string& message)
{
    // Clients decide how often these happen, so they are throttled.
    if (log_throttles_.connections.Allow())
    {
        log_.Write(level, message + log_throttles_.connections.TakeSuppressedNote());
    }
}

void RemoteInputListener::OnSessionEnded(RemoteInputClientId client_id)
{
    sessions_.erase(client_id);
}
