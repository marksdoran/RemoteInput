// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: http://www.viva64.com
#include "RemoteInputServer.h"

#include <cstddef>
#include <format>
#include <thread>
#include <utility>

#include "ExtLibs/asio/include/asio.hpp"

#include "FALCLIB/INCLUDE/ehandler.h"

#include "RemoteInputAccessPolicy.h"
#include "RemoteInputListener.h"
#include "RemoteInputLog.h"

namespace
{
// The local addresses to listen on. Each address family gets a socket of its own.
std::vector<asio::ip::address> ListenAddresses(const RemoteInputAccessPolicy& policy)
{
    if (policy.AllowsLanClients())
    {
        return { asio::ip::address_v4::any(), asio::ip::address_v6::any() };
    }
    // Listening on the loopback addresses only means other computers cannot even open a connection. Both families
    // are needed because "localhost" often resolves to ::1 first, on Windows in particular.
    return { asio::ip::address_v4::loopback(), asio::ip::address_v6::loopback() };
}
} // namespace

// Everything that exists only while the server is running. The members are declared in the order they depend on
// each other: the listener's sockets must be destroyed before the io_context they belong to.
struct RemoteInputServer::Network
{
    Network(RemoteInputAccessPolicy policy, const RemoteInputLimits& limits, RemoteInputClientId first_client_id,
        RemoteInputQueue& queue, RemoteInputLog& log) :
        io_context(1),   // concurrency hint: only the network thread runs this io_context
        listener(io_context, std::move(policy), limits, first_client_id, queue, log)
    {
    }

    asio::io_context io_context;
    RemoteInputListener listener;
    std::thread thread;
};

RemoteInputServer::RemoteInputServer(RemoteInputLog& log, const RemoteInputLimits& limits) :
    log_(log),
    limits_(limits),
    queue_(limits.max_queued_commands)
{
}

RemoteInputServer::~RemoteInputServer()
{
    Stop();
}

bool RemoteInputServer::Start(const RemoteInputSettings& settings)
{
    INT3_ASSERT(network_ == nullptr);   // Start() called again without Stop()
    if (network_ != nullptr)
    {
        return true;
    }

    if (settings.mode == RemoteInputMode::Disabled)
    {
        log_.Write(RemoteInputLogLevel::Info, "Disabled by configuration");
        return false;
    }

    RemoteInputAccessPolicy policy = RemoteInputAccessPolicy::Create(settings, log_);
    const std::vector<asio::ip::address> addresses = ListenAddresses(policy);
    auto network = std::make_unique<Network>(std::move(policy), limits_, next_client_id_, queue_, log_);
    if (!network->listener.Open(addresses, settings.port))
    {
        log_.Write(RemoteInputLogLevel::Error, std::format("Not started: could not listen on port {}", settings.port));
        return false;
    }

    // Logged before the network thread starts, so that it comes before anything that thread logs.
    const bool is_lan_mode = settings.mode == RemoteInputMode::Lan;
    const bool listens_on_lan = addresses.front().is_unspecified();
    log_.Write(RemoteInputLogLevel::Info, std::format("Listening on {} ({}{})", network->listener.Endpoints(),
        RemoteInputModeName(settings.mode), is_lan_mode && !listens_on_lan ? ", but no LAN client is allowed" : ""));

    // Queue the first accepts before the thread starts: io_context::run() returns at once if it has no work.
    network->listener.StartAccepting();
    network->thread = std::thread([&io_context = network->io_context]
        {
            io_context.run();
        });
    network_ = std::move(network);
    return true;
}

void RemoteInputServer::Stop()
{
    if (network_ == nullptr)
    {
        return;
    }
    // Stop() waits for the network thread, so calling it from that thread (e.g. from a log callback) would never
    // return.
    const bool is_network_thread = std::this_thread::get_id() == network_->thread.get_id();
    INT3_ASSERT(!is_network_thread);
    if (is_network_thread)
    {
        return;
    }

    // The sockets belong to the network thread, so they are closed there. Once the last client's read has
    // completed, run() has nothing left to do and the thread exits.
    asio::post(network_->io_context, [&listener = network_->listener]
        {
            listener.Close();
        });
    network_->thread.join();

    // The thread has exited, so the listener can be read here safely.
    next_client_id_ = network_->listener.NextClientId();
    const std::size_t unlogged = network_->listener.TakeUnloggedMessageCount();
    network_.reset();

    if (unlogged == 1)
    {
        log_.Write(RemoteInputLogLevel::Info, "1 message about clients was not logged, to keep the log readable");
    }
    else if (unlogged > 1)
    {
        log_.Write(RemoteInputLogLevel::Info, std::format(
            "{} messages about clients were not logged, to keep the log readable", unlogged));
    }
    log_.Write(RemoteInputLogLevel::Info, "Stopped");
}

bool RemoteInputServer::IsRunning() const
{
    return network_ != nullptr;
}

std::uint16_t RemoteInputServer::Port() const
{
    if (network_ == nullptr)
    {
        return 0;
    }
    return network_->listener.Port();
}

void RemoteInputServer::DrainRemoteInput(std::vector<RemoteInputCommand>& commands)
{
    queue_.DrainInto(commands);
}
