#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "ExtLibs/asio/include/asio.hpp"

class RemoteInputLog;

// Looks up the host names in the allowed client list, so that a device can be allowed by name even though its
// address may change, e.g. a phone that gets a new address from DHCP.
//
// The names are looked up only when needed: when a local network client connects from an address that is not
// allowed. Lookups are at least the lookup interval apart, so a refused client that keeps reconnecting cannot turn
// into a stream of lookups; a request that comes too soon is served by a lookup at the end of the interval. ASIO
// performs the lookups on one helper thread of its own, one name after another, and delivers each result to the
// network thread, where all of this class's member functions run.
class RemoteInputHostResolver final
{
public:
    // Called with the local network addresses the names resolved to, once all of them have been looked up. Other
    // addresses could never be allowed, so they are logged and left out.
    using ResultHandler = std::function<void(const std::vector<asio::ip::address>&)>;

    RemoteInputHostResolver(asio::io_context& io_context, std::vector<std::string> host_names,
        std::chrono::steady_clock::duration lookup_interval, RemoteInputLog& log, ResultHandler on_result);

    RemoteInputHostResolver(const RemoteInputHostResolver&) = delete;
    RemoteInputHostResolver& operator=(const RemoteInputHostResolver&) = delete;

    // Asks for every name to be looked up again. Returns true if a result will follow: a lookup has started, one
    // was already running, or one is due at the end of the lookup interval. Returns false only if there are no
    // names to look up, or after Close().
    bool LookUp();

    // Cancels lookups that have not started and discards every result still to come. A lookup the OS is already
    // performing cannot be interrupted, so the network thread's run() returns only once that lookup has finished.
    void Close();

private:
    void StartLookups();
    void OnLookedUp(std::size_t index, const asio::error_code& error,
        const asio::ip::tcp::resolver::results_type& results);

    asio::ip::tcp::resolver resolver_;
    asio::steady_timer delay_timer_;                    // waits for the end of the lookup interval
    const std::vector<std::string> host_names_;
    const std::chrono::steady_clock::duration lookup_interval_;
    RemoteInputLog& log_;
    ResultHandler on_result_;

    std::vector<std::string> last_outcomes_;           // what each name last resolved to, so only changes are logged
    std::vector<asio::ip::address> found_addresses_;   // collected by the lookup in progress
    std::size_t lookups_in_progress_ = 0;
    std::optional<std::chrono::steady_clock::time_point> last_lookup_finished_;
    bool is_lookup_delayed_ = false;
    bool is_closed_ = false;
};
