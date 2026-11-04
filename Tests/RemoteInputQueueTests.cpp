// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: http://www.viva64.com
#include <chrono>
#include <cstddef>
#include <format>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "RemoteInputQueue.h"

namespace
{
// Each command as the client would have sent it, e.g. "SimEject_Press".
std::vector<std::string> Describe(const std::vector<RemoteInputCommand>& commands)
{
    std::vector<std::string> descriptions;
    descriptions.reserve(commands.size());
    for (const RemoteInputCommand& command : commands)
    {
        descriptions.push_back(MakeRemoteInputCommand(command.callback, command.action));
    }
    return descriptions;
}
} // namespace

TEST(RemoteInputQueue, DrainsCommandsInOrder)
{
    RemoteInputQueue queue(10);
    EXPECT_TRUE(queue.TryPush(1, "SimEject", RemoteInputAction::Press));
    EXPECT_TRUE(queue.TryPush(2, "AFBrakesToggle", RemoteInputAction::PressAndRelease));
    EXPECT_TRUE(queue.TryPush(1, "SimEject", RemoteInputAction::Release));

    std::vector<RemoteInputCommand> commands;
    queue.DrainInto(commands);

    ASSERT_EQ(Describe(commands),
        (std::vector<std::string>{ "SimEject_Press", "AFBrakesToggle", "SimEject_Release" }));
    EXPECT_EQ(commands[0].client_id, 1u);
    EXPECT_EQ(commands[1].client_id, 2u);
    EXPECT_EQ(commands[2].client_id, 1u);
}

TEST(RemoteInputQueue, DrainReplacesWhatTheVectorHeld)
{
    RemoteInputQueue queue(10);
    std::vector<RemoteInputCommand> commands(3);

    queue.DrainInto(commands);
    EXPECT_TRUE(commands.empty());

    EXPECT_TRUE(queue.TryPush(1, "A", RemoteInputAction::PressAndRelease));
    queue.DrainInto(commands);
    EXPECT_EQ(Describe(commands), std::vector<std::string>{ "A" });

    queue.DrainInto(commands);
    EXPECT_TRUE(commands.empty());
}

TEST(RemoteInputQueue, DropsCommandsBeyondTheLimit)
{
    RemoteInputQueue queue(2);
    EXPECT_TRUE(queue.TryPush(1, "A", RemoteInputAction::PressAndRelease));
    EXPECT_TRUE(queue.TryPush(1, "B", RemoteInputAction::PressAndRelease));
    EXPECT_FALSE(queue.TryPush(1, "C", RemoteInputAction::PressAndRelease));

    std::vector<RemoteInputCommand> commands;
    queue.DrainInto(commands);
    EXPECT_EQ(Describe(commands), (std::vector<std::string>{ "A", "B" }));

    // Draining makes room again.
    EXPECT_TRUE(queue.TryPush(1, "D", RemoteInputAction::PressAndRelease));
    queue.DrainInto(commands);
    EXPECT_EQ(Describe(commands), std::vector<std::string>{ "D" });
}

TEST(RemoteInputQueue, PushEvenIfFullIgnoresTheLimit)
{
    RemoteInputQueue queue(1);
    EXPECT_TRUE(queue.TryPush(1, "A", RemoteInputAction::Press));
    EXPECT_FALSE(queue.TryPush(1, "B", RemoteInputAction::PressAndRelease));
    queue.PushEvenIfFull(1, "A", RemoteInputAction::Release);

    std::vector<RemoteInputCommand> commands;
    queue.DrainInto(commands);
    EXPECT_EQ(Describe(commands), (std::vector<std::string>{ "A_Press", "A_Release" }));
}

TEST(RemoteInputQueue, RecordsWhenEachCommandArrived)
{
    RemoteInputQueue queue(10);
    const auto before = std::chrono::steady_clock::now();
    EXPECT_TRUE(queue.TryPush(1, "A", RemoteInputAction::PressAndRelease));
    queue.PushEvenIfFull(1, "B", RemoteInputAction::Release);
    const auto after = std::chrono::steady_clock::now();

    std::vector<RemoteInputCommand> commands;
    queue.DrainInto(commands);
    ASSERT_EQ(commands.size(), 2u);
    for (const RemoteInputCommand& command : commands)
    {
        EXPECT_GE(command.received_at, before);
        EXPECT_LE(command.received_at, after);
    }
}

// Several producers push while the consumer drains: nothing may be lost, duplicated or reordered within a
// producer, and the limit must hold. Also gives ThreadSanitizer something to look at.
TEST(RemoteInputQueue, LosesNothingWithConcurrentProducers)
{
    constexpr std::size_t producer_count = 4;
    constexpr std::size_t commands_per_producer = 2000;
    constexpr std::size_t total_commands = producer_count * commands_per_producer;
    RemoteInputQueue queue(total_commands);

    std::vector<std::jthread> producers;
    for (std::size_t producer = 0; producer < producer_count; ++producer)
    {
        producers.emplace_back([&queue, producer]
            {
                for (std::size_t sequence = 0; sequence < commands_per_producer; ++sequence)
                {
                    EXPECT_TRUE(queue.TryPush(static_cast<RemoteInputClientId>(producer), std::format("C{}", sequence),
                        RemoteInputAction::PressAndRelease));
                }
            });
    }

    std::vector<std::size_t> next_sequence(producer_count, 0);
    std::size_t received = 0;
    std::vector<RemoteInputCommand> commands;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (received < total_commands && std::chrono::steady_clock::now() < deadline)
    {
        queue.DrainInto(commands);
        for (const RemoteInputCommand& command : commands)
        {
            std::size_t& expected = next_sequence[command.client_id];
            EXPECT_EQ(command.callback, std::format("C{}", expected));
            ++expected;
        }
        received += commands.size();
    }

    EXPECT_EQ(received, total_commands);
}

TEST(RemoteInputQueue, LimitHoldsWithConcurrentProducers)
{
    constexpr std::size_t limit = 100;
    RemoteInputQueue queue(limit);

    std::vector<std::jthread> producers;
    for (int producer = 0; producer < 4; ++producer)
    {
        producers.emplace_back([&queue]
            {
                for (int attempt = 0; attempt < 1000; ++attempt)
                {
                    queue.TryPush(1, "A", RemoteInputAction::PressAndRelease);
                }
            });
    }
    producers.clear();   // joins them

    std::vector<RemoteInputCommand> commands;
    queue.DrainInto(commands);
    EXPECT_EQ(commands.size(), limit);
}
