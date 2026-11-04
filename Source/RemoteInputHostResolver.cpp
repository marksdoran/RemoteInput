// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: http://www.viva64.com
#include "RemoteInputHostResolver.h"

#include <format>
#include <utility>

#include "RemoteInputAccessPolicy.h"
#include "RemoteInputLog.h"

RemoteInputHostResolver::RemoteInputHostResolver(asio::io_context& io_context, std::vector<std::string> host_names,
    std::chrono::steady_clock::duration lookup_interval, RemoteInputLog& log, ResultHandler on_result) :
    resolver_(io_context),
    delay_timer_(io_context),
    host_names_(std::move(host_names)),
    lookup_interval_(lookup_interval),
    log_(log),
    on_result_(std::move(on_result)),
    last_outcomes_(host_names_.size())
{
}

bool RemoteInputHostResolver::LookUp()
{
    if (host_names_.empty() || is_closed_)
    {
        return false;
    }
    if (lookups_in_progress_ > 0 || is_lookup_delayed_)
    {
        return true;
    }

    const auto now = std::chrono::steady_clock::now();
    if (last_lookup_finished_.has_value() && now - *last_lookup_finished_ < lookup_interval_)
    {
        is_lookup_delayed_ = true;
        delay_timer_.expires_at(*last_lookup_finished_ + lookup_interval_);
        delay_timer_.async_wait([this](const asio::error_code& error)
            {
                is_lookup_delayed_ = false;
                if (!error && !is_closed_)
                {
                    StartLookups();
                }
            });
        return true;
    }

    StartLookups();
    return true;
}

void RemoteInputHostResolver::Close()
{
    is_closed_ = true;
    delay_timer_.cancel();
    resolver_.cancel();
}

void RemoteInputHostResolver::StartLookups()
{
    found_addresses_.clear();
    lookups_in_progress_ = host_names_.size();
    for (std::size_t index = 0; index < host_names_.size(); ++index)
    {
        // Only the addresses matter: the numeric service keeps the resolver from looking up a port name, and
        // address_configured skips address families this computer cannot use anyway, which can save a slow query.
        resolver_.async_resolve(host_names_[index], "0",
            asio::ip::resolver_base::numeric_service | asio::ip::resolver_base::address_configured,
            [this, index](const asio::error_code& error, const asio::ip::tcp::resolver::results_type& results)
            {
                OnLookedUp(index, error, results);
            });
    }
}

void RemoteInputHostResolver::OnLookedUp(std::size_t index, const asio::error_code& error,
    const asio::ip::tcp::resolver::results_type& results)
{
    --lookups_in_progress_;
    if (is_closed_)
    {
        return;
    }

    // Describe the outcome, keeping the addresses that can be allowed.
    std::string outcome;
    if (error)
    {
        outcome = std::format("could not be looked up: {}", error.message());
    }
    else if (results.empty())
    {
        outcome = "resolves to no addresses";
    }
    else
    {
        outcome = "resolves to ";
        bool is_first = true;
        for (const auto& entry : results)
        {
            const asio::ip::address address = NormalizeAddress(entry.endpoint().address());
            outcome += is_first ? "" : ", ";
            outcome += address.to_string();
            is_first = false;
            if (IsLanAddress(address))
            {
                found_addresses_.push_back(address);
            }
            else if (!address.is_loopback())   // programs on this computer can connect anyway
            {
                outcome += " (not a local network address, so not allowed)";
            }
        }
    }

    // The names are looked up again whenever an unrecognised local client connects, so only changes are logged.
    if (outcome != last_outcomes_[index])
    {
        log_.Write(error ? RemoteInputLogLevel::Warning : RemoteInputLogLevel::Info, std::format(
            "Allowed client {} {}", QuoteForRemoteInputLog(host_names_[index]), outcome));
        last_outcomes_[index] = std::move(outcome);
    }

    if (lookups_in_progress_ == 0)
    {
        last_lookup_finished_ = std::chrono::steady_clock::now();
        on_result_(found_addresses_);
    }
}
