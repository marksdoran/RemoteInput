// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: http://www.viva64.com

// Tests of the host name lookups, run on the test's own thread. Numeric "names" resolve without a name server, so
// most of these tests work on any computer that has a network connection.

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "ExtLibs/asio/include/asio.hpp"

#include "RemoteInputHostResolver.h"
#include "RemoteInputLog.h"
#include "TestLog.h"

using namespace std::chrono_literals;

namespace
{
using Addresses = std::vector<asio::ip::address>;

asio::ip::address Address(std::string_view text)
{
    asio::error_code error;
    const asio::ip::address address = asio::ip::make_address(text, error);
    EXPECT_FALSE(error) << "test address " << text << " does not parse";
    return address;
}

// The resolver only looks up address families the computer has configured, and on some systems loopback does not
// count, so lookups need a network connection.
bool HasNetworkAddress()
{
    asio::io_context io_context;
    asio::ip::udp::socket socket(io_context);
    asio::error_code error;
    socket.open(asio::ip::udp::v4(), error);
    socket.connect(asio::ip::udp::endpoint(Address("192.0.2.1"), 9), error);   // sends nothing
    const asio::ip::address address = socket.local_endpoint(error).address();
    return !error && !address.is_loopback() && !address.is_unspecified();
}

class RemoteInputHostResolverTest : public testing::Test
{
protected:
    void SetUp() override
    {
        if (!HasNetworkAddress())
        {
            GTEST_SKIP() << "this computer has no network connection";
        }
    }

    RemoteInputHostResolver& MakeResolver(std::vector<std::string> host_names,
        std::chrono::steady_clock::duration lookup_interval = std::chrono::steady_clock::duration::zero())
    {
        resolver_.emplace(io_context_, std::move(host_names), lookup_interval, log_,
            [this](const Addresses& addresses)
            {
                results_.push_back(addresses);
            });
        return *resolver_;
    }

    // Handles whatever the lookups deliver, returning once there is nothing left to wait for.
    void Run()
    {
        io_context_.restart();
        io_context_.run();
    }

    asio::io_context io_context_;
    TestLog log_;
    std::vector<Addresses> results_;
    std::optional<RemoteInputHostResolver> resolver_;   // last, so it goes before what it uses
};
} // namespace

TEST_F(RemoteInputHostResolverTest, KeepsOnlyLocalNetworkAddresses)
{
    RemoteInputHostResolver& resolver = MakeResolver({ "192.168.1.5", "8.8.8.8", "127.0.0.1" });

    ASSERT_TRUE(resolver.LookUp());
    Run();

    ASSERT_EQ(results_.size(), 1u) << log_.Dump();
    EXPECT_EQ(results_[0], Addresses{ Address("192.168.1.5") });
    EXPECT_TRUE(log_.Contains(RemoteInputLogLevel::Info, "Allowed client \"192.168.1.5\" resolves to 192.168.1.5"))
        << log_.Dump();
    EXPECT_TRUE(log_.Contains(RemoteInputLogLevel::Info,
        "Allowed client \"8.8.8.8\" resolves to 8.8.8.8 (not a local network address, so not allowed)")) << log_.Dump();

    // Loopback is allowed anyway, so it is neither kept nor called a problem.
    EXPECT_TRUE(log_.Contains(RemoteInputLogLevel::Info, "Allowed client \"127.0.0.1\" resolves to 127.0.0.1"))
        << log_.Dump();
    EXPECT_FALSE(log_.Contains(RemoteInputLogLevel::Info, "127.0.0.1 (not")) << log_.Dump();
}

TEST_F(RemoteInputHostResolverTest, WarnsAboutNamesThatCannotBeLookedUp)
{
    RemoteInputHostResolver& resolver = MakeResolver({ "no-such-host.invalid", "10.1.2.3" });

    ASSERT_TRUE(resolver.LookUp());
    Run();

    // The name that failed does not stop the other one from being allowed.
    ASSERT_EQ(results_.size(), 1u) << log_.Dump();
    EXPECT_EQ(results_[0], Addresses{ Address("10.1.2.3") });
    EXPECT_TRUE(log_.Contains(RemoteInputLogLevel::Warning,
        "Allowed client \"no-such-host.invalid\" could not be looked up: ")) << log_.Dump();
}

TEST_F(RemoteInputHostResolverTest, LogsWhatANameResolvesToOnlyWhenThatChanges)
{
    RemoteInputHostResolver& resolver = MakeResolver({ "192.168.1.5" });

    ASSERT_TRUE(resolver.LookUp());
    Run();
    ASSERT_TRUE(resolver.LookUp());
    Run();

    EXPECT_EQ(results_.size(), 2u) << log_.Dump();
    EXPECT_EQ(log_.Count(RemoteInputLogLevel::Info, "\"192.168.1.5\" resolves to"), 1u) << log_.Dump();
}

TEST_F(RemoteInputHostResolverTest, PutsOffALookupThatComesTooSoon)
{
    constexpr std::chrono::milliseconds lookup_interval = 300ms;
    RemoteInputHostResolver& resolver = MakeResolver({ "192.168.1.5" }, lookup_interval);
    ASSERT_TRUE(resolver.LookUp());
    Run();
    ASSERT_EQ(results_.size(), 1u) << log_.Dump();

    // Both requests are answered by one lookup, at the end of the interval.
    const auto asked_at = std::chrono::steady_clock::now();
    EXPECT_TRUE(resolver.LookUp());
    EXPECT_TRUE(resolver.LookUp());
    Run();

    EXPECT_EQ(results_.size(), 2u) << log_.Dump();
    EXPECT_GE(std::chrono::steady_clock::now() - asked_at, lookup_interval / 2);
}

TEST_F(RemoteInputHostResolverTest, HasNothingToDoWithoutNames)
{
    RemoteInputHostResolver& resolver = MakeResolver({});

    EXPECT_FALSE(resolver.LookUp());
    Run();
    EXPECT_TRUE(results_.empty());
}

TEST_F(RemoteInputHostResolverTest, CloseDiscardsResultsStillToCome)
{
    RemoteInputHostResolver& resolver = MakeResolver({ "192.168.1.5" });

    ASSERT_TRUE(resolver.LookUp());
    resolver.Close();
    Run();

    EXPECT_TRUE(results_.empty()) << log_.Dump();
    EXPECT_FALSE(log_.Contains(RemoteInputLogLevel::Info, "resolves to")) << log_.Dump();
    EXPECT_FALSE(resolver.LookUp());
}

TEST_F(RemoteInputHostResolverTest, CloseCancelsALookupThatWasPutOff)
{
    RemoteInputHostResolver& resolver = MakeResolver({ "192.168.1.5" }, 1h);
    ASSERT_TRUE(resolver.LookUp());
    Run();
    ASSERT_TRUE(resolver.LookUp());   // put off for an hour

    const auto closed_at = std::chrono::steady_clock::now();
    resolver.Close();
    Run();

    EXPECT_EQ(results_.size(), 1u) << log_.Dump();
    EXPECT_LT(std::chrono::steady_clock::now() - closed_at, 5s);
}
