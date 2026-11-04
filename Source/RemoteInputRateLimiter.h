#pragma once

#include <chrono>
#include <cstddef>
#include <string>

// How much a new rate limiter allows straight away.
enum class RemoteInputInitialAllowance
{
    FullBurst,   // a whole burst
    Empty,       // nothing until the allowance has built up at the limiter's rate
};

// A token bucket: allows bursts of up to `burst` events, refilled at `events_per_second`. Used to cap what a
// client can make the server do, however fast it sends. Not thread-safe; each user owns its own.
class RemoteInputRateLimiter final
{
public:
    // A burst of less than one event could never allow anything, so it is raised to one.
    RemoteInputRateLimiter(double burst, double events_per_second,
        RemoteInputInitialAllowance initial_allowance = RemoteInputInitialAllowance::FullBurst,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    // Takes one event's worth of allowance if there is any left, and returns whether it did.
    bool TryAcquire(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    // How long until TryAcquire() can next succeed, or zero if it can now. A limiter that never refills (a rate of
    // zero) reports an hour, so that a caller can simply check again then.
    std::chrono::steady_clock::duration TimeUntilAvailable(
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

private:
    void Refill(std::chrono::steady_clock::time_point now);

    const double burst_;
    const double events_per_second_;
    double tokens_;
    std::chrono::steady_clock::time_point last_refill_;
};

// Keeps the messages that clients can cause from flooding the log: lets the first twenty through, then about one a
// second, and counts the ones it holds back so that the next one can say so.
class RemoteInputLogThrottle final
{
public:
    explicit RemoteInputLogThrottle(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    // Returns true if a message may be logged now. Otherwise counts the message as left out.
    bool Allow(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());

    // How many messages have been left out since this was last called.
    std::size_t TakeSuppressedCount();

    // For the end of an allowed message: " (3 earlier messages about clients were not logged)", or "" if none were
    // left out. The throttles are shared by every client, so the messages left out need not be about the same one.
    std::string TakeSuppressedNote();

private:
    RemoteInputRateLimiter limiter_;
    std::size_t suppressed_ = 0;
};

// The throttles for the messages that clients can cause. The listener owns them and shares them with every session,
// so that a client cannot reset them by reconnecting.
struct RemoteInputClientLogThrottles
{
    RemoteInputLogThrottle connections;   // connections refused, held, made and ended
    RemoteInputLogThrottle lines;         // invalid lines and dropped commands
};
