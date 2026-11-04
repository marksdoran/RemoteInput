// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: http://www.viva64.com
#include "RemoteInputRateLimiter.h"

#include <format>

namespace
{
constexpr double log_burst = 20.0;
constexpr double log_messages_per_second = 1.0;
} // namespace

RemoteInputRateLimiter::RemoteInputRateLimiter(double burst, double events_per_second,
    RemoteInputInitialAllowance initial_allowance, std::chrono::steady_clock::time_point now) :
    burst_(burst < 1.0 ? 1.0 : burst),
    events_per_second_(events_per_second < 0.0 ? 0.0 : events_per_second),
    tokens_(initial_allowance == RemoteInputInitialAllowance::FullBurst ? burst_ : 0.0),
    last_refill_(now)
{
}

bool RemoteInputRateLimiter::TryAcquire(std::chrono::steady_clock::time_point now)
{
    Refill(now);
    if (tokens_ < 1.0)
    {
        return false;
    }
    tokens_ -= 1.0;
    return true;
}

std::chrono::steady_clock::duration RemoteInputRateLimiter::TimeUntilAvailable(
    std::chrono::steady_clock::time_point now)
{
    Refill(now);
    if (tokens_ >= 1.0)
    {
        return std::chrono::steady_clock::duration::zero();
    }
    if (events_per_second_ <= 0.0)
    {
        return std::chrono::hours(1);
    }
    const std::chrono::duration<double> wait((1.0 - tokens_) / events_per_second_);
    return std::chrono::ceil<std::chrono::steady_clock::duration>(wait);
}

void RemoteInputRateLimiter::Refill(std::chrono::steady_clock::time_point now)
{
    // A time earlier than the last one adds nothing, so the allowance never goes backwards.
    if (now > last_refill_)
    {
        const std::chrono::duration<double> elapsed = now - last_refill_;
        tokens_ += elapsed.count() * events_per_second_;
        if (tokens_ > burst_)
        {
            tokens_ = burst_;
        }
        last_refill_ = now;
    }
}

RemoteInputLogThrottle::RemoteInputLogThrottle(std::chrono::steady_clock::time_point now) :
    limiter_(log_burst, log_messages_per_second, RemoteInputInitialAllowance::FullBurst, now)
{
}

bool RemoteInputLogThrottle::Allow(std::chrono::steady_clock::time_point now)
{
    if (limiter_.TryAcquire(now))
    {
        return true;
    }
    ++suppressed_;
    return false;
}

std::size_t RemoteInputLogThrottle::TakeSuppressedCount()
{
    const std::size_t suppressed = suppressed_;
    suppressed_ = 0;
    return suppressed;
}

std::string RemoteInputLogThrottle::TakeSuppressedNote()
{
    const std::size_t suppressed = TakeSuppressedCount();
    if (suppressed == 0)
    {
        return {};
    }
    if (suppressed == 1)
    {
        return " (1 earlier message about clients was not logged)";
    }
    return std::format(" ({} earlier messages about clients were not logged)", suppressed);
}
