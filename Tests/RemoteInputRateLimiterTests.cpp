// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: http://www.viva64.com
#include <chrono>
#include <cstddef>

#include "gtest/gtest.h"

#include "RemoteInputRateLimiter.h"

using namespace std::chrono_literals;

namespace
{
// The tests pass their own clock readings, so the results never depend on how fast the computer is. The times and
// rates are chosen so that the arithmetic is exact in floating point.
const std::chrono::steady_clock::time_point start = std::chrono::steady_clock::time_point() + 1h;

std::size_t CountAcquired(RemoteInputRateLimiter& limiter, std::chrono::steady_clock::time_point now,
    std::size_t attempts)
{
    std::size_t acquired = 0;
    for (std::size_t attempt = 0; attempt < attempts; ++attempt)
    {
        if (limiter.TryAcquire(now))
        {
            ++acquired;
        }
    }
    return acquired;
}

std::size_t CountAllowed(RemoteInputLogThrottle& throttle, std::chrono::steady_clock::time_point now,
    std::size_t messages)
{
    std::size_t allowed = 0;
    for (std::size_t message = 0; message < messages; ++message)
    {
        if (throttle.Allow(now))
        {
            ++allowed;
        }
    }
    return allowed;
}
} // namespace

TEST(RemoteInputRateLimiter, AllowsABurstThenRefillsAtItsRate)
{
    RemoteInputRateLimiter limiter(5.0, 4.0, RemoteInputInitialAllowance::FullBurst, start);

    EXPECT_EQ(CountAcquired(limiter, start, 8), 5u);
    EXPECT_FALSE(limiter.TryAcquire(start + 125ms));   // half an event's worth has built up
    EXPECT_TRUE(limiter.TryAcquire(start + 250ms));    // the other half
    EXPECT_FALSE(limiter.TryAcquire(start + 250ms));
    EXPECT_EQ(CountAcquired(limiter, start + 1250ms, 8), 4u);
}

TEST(RemoteInputRateLimiter, CanStartEmpty)
{
    RemoteInputRateLimiter limiter(5.0, 4.0, RemoteInputInitialAllowance::Empty, start);

    EXPECT_FALSE(limiter.TryAcquire(start));
    EXPECT_FALSE(limiter.TryAcquire(start + 125ms));
    EXPECT_TRUE(limiter.TryAcquire(start + 250ms));
    EXPECT_EQ(CountAcquired(limiter, start + 24h, 10), 5u);
}

TEST(RemoteInputRateLimiter, NeverSavesUpMoreThanItsBurst)
{
    RemoteInputRateLimiter limiter(3.0, 4.0, RemoteInputInitialAllowance::FullBurst, start);

    EXPECT_EQ(CountAcquired(limiter, start + 24h, 10), 3u);
}

TEST(RemoteInputRateLimiter, AlwaysAllowsAtLeastOneEventAtATime)
{
    // A burst below one would otherwise never allow anything, however long it waited.
    RemoteInputRateLimiter limiter(0.5, 0.5, RemoteInputInitialAllowance::FullBurst, start);

    EXPECT_TRUE(limiter.TryAcquire(start));
    EXPECT_FALSE(limiter.TryAcquire(start + 1s));
    EXPECT_TRUE(limiter.TryAcquire(start + 2s));
}

TEST(RemoteInputRateLimiter, SaysHowLongUntilTheNextEvent)
{
    RemoteInputRateLimiter limiter(2.0, 4.0, RemoteInputInitialAllowance::FullBurst, start);

    EXPECT_EQ(limiter.TimeUntilAvailable(start), std::chrono::steady_clock::duration::zero());
    EXPECT_EQ(CountAcquired(limiter, start, 2), 2u);
    EXPECT_EQ(limiter.TimeUntilAvailable(start), 250ms);
    EXPECT_EQ(limiter.TimeUntilAvailable(start + 125ms), 125ms);
    EXPECT_EQ(limiter.TimeUntilAvailable(start + 250ms), std::chrono::steady_clock::duration::zero());
    EXPECT_TRUE(limiter.TryAcquire(start + 250ms));
}

TEST(RemoteInputRateLimiter, ReportsAnHourWhenItNeverRefills)
{
    RemoteInputRateLimiter limiter(1.0, 0.0, RemoteInputInitialAllowance::Empty, start);

    EXPECT_EQ(limiter.TimeUntilAvailable(start), std::chrono::hours(1));
    EXPECT_FALSE(limiter.TryAcquire(start + 24h));
}

TEST(RemoteInputRateLimiter, IgnoresTimesBeforeTheLastOne)
{
    RemoteInputRateLimiter limiter(1.0, 1.0, RemoteInputInitialAllowance::FullBurst, start);

    EXPECT_TRUE(limiter.TryAcquire(start));
    EXPECT_FALSE(limiter.TryAcquire(start - 10s));   // earlier than before: adds nothing, and breaks nothing
    EXPECT_TRUE(limiter.TryAcquire(start + 1s));
}

TEST(RemoteInputLogThrottle, LetsTwentyThroughThenOneASecond)
{
    RemoteInputLogThrottle throttle(start);

    EXPECT_EQ(CountAllowed(throttle, start, 25), 20u);
    EXPECT_FALSE(throttle.Allow(start + 500ms));
    EXPECT_TRUE(throttle.Allow(start + 1s));
    EXPECT_EQ(CountAllowed(throttle, start + 4s, 10), 3u);
}

TEST(RemoteInputLogThrottle, CountsTheMessagesItHoldsBack)
{
    RemoteInputLogThrottle throttle(start);

    EXPECT_EQ(CountAllowed(throttle, start, 23), 20u);
    EXPECT_EQ(throttle.TakeSuppressedNote(), " (3 earlier messages about clients were not logged)");
    EXPECT_EQ(throttle.TakeSuppressedNote(), "");

    EXPECT_FALSE(throttle.Allow(start));
    EXPECT_EQ(throttle.TakeSuppressedNote(), " (1 earlier message about clients was not logged)");

    EXPECT_FALSE(throttle.Allow(start));
    EXPECT_FALSE(throttle.Allow(start));
    EXPECT_EQ(throttle.TakeSuppressedCount(), 2u);
    EXPECT_EQ(throttle.TakeSuppressedCount(), 0u);
}
