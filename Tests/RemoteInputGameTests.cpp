// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: http://www.viva64.com

// Tests of the game-side glue (Game/RemoteInputGame.h), run against the stand-in game functions in EngineStubs.

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "ExtLibs/asio/include/asio.hpp"

#include "GameStubs.h"
#include "RemoteInputGame.h"

// The game's config variables, which the glue reads. Each test sets the ones it needs.
int g_nCallbackSocketMode = 0;
int g_nCallbackSocketPortNum = 0;
char g_sCallbackSocketRemoteClients[256] = "";

namespace
{
// A port that was free a moment ago. The config variable cannot ask for "any free port" the way the tests of the
// server itself do.
std::uint16_t FindFreePort()
{
    asio::io_context io_context;
    asio::ip::tcp::acceptor acceptor(io_context);
    asio::error_code error;
    acceptor.open(asio::ip::tcp::v4(), error);
    acceptor.bind(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(), 0), error);
    return acceptor.local_endpoint(error).port();
}

// Collects calls the way the input loop does, once per "frame", until `count` have arrived or 5 seconds pass.
std::vector<RemoteInputCall> GetCallsUntil(std::size_t count)
{
    std::vector<RemoteInputCall> received;
    std::vector<RemoteInputCall> frame;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (received.size() < count && std::chrono::steady_clock::now() < deadline)
    {
        GetRemoteInput(frame);
        received.insert(received.end(), frame.begin(), frame.end());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return received;
}

std::size_t CountPrinted(const std::vector<std::string>& lines, std::string_view fragment)
{
    return static_cast<std::size_t>(std::ranges::count_if(lines, [fragment](const std::string& line)
        {
            return line.find(fragment) != std::string::npos;
        }));
}

bool WasPrinted(const std::vector<std::string>& lines, std::string_view fragment)
{
    return CountPrinted(lines, fragment) > 0;
}

// Stands in for a companion app, connecting to the port the config variable names.
class TestClient final
{
public:
    TestClient() :
        socket_(io_context_)
    {
        asio::error_code error;
        socket_.connect(asio::ip::tcp::endpoint(asio::ip::address_v4::loopback(),
            static_cast<std::uint16_t>(g_nCallbackSocketPortNum)), error);
        EXPECT_FALSE(error) << "connect failed: " << error.message();
    }

    void Send(std::string_view data)
    {
        asio::error_code error;
        asio::write(socket_, asio::buffer(data), error);
        EXPECT_FALSE(error) << "send failed: " << error.message();
    }

private:
    asio::io_context io_context_;
    asio::ip::tcp::socket socket_;
};

class RemoteInputGameTest : public testing::Test
{
protected:
    void SetUp() override
    {
        StartRecordingMonoPrint();
        g_nCallbackSocketMode = 1;
        g_nCallbackSocketPortNum = FindFreePort();
        g_sCallbackSocketRemoteClients[0] = '\0';
    }

    void TearDown() override
    {
        StopRemoteInput();
        std::vector<RemoteInputCall> leftovers;
        GetRemoteInput(leftovers);   // so the next test starts with nothing waiting
    }
};
} // namespace

TEST_F(RemoteInputGameTest, StaysOffWhenTheConfigSaysSo)
{
    g_nCallbackSocketMode = 0;

    EXPECT_FALSE(StartRemoteInput());
    std::vector<RemoteInputCall> calls(1);
    GetRemoteInput(calls);
    EXPECT_TRUE(calls.empty());
}

TEST_F(RemoteInputGameTest, LooksUpCallbacksAndLogsUnknownNames)
{
    ASSERT_TRUE(StartRemoteInput()) << "see the log above";

    TestClient client;
    client.Send("SimEject\nNoSuchCallback\nSimPickle_Press\nSimPickle_Release\n");

    const std::vector<RemoteInputCall> calls = GetCallsUntil(3);
    ASSERT_EQ(calls.size(), 3u);
    EXPECT_EQ(calls[0].command.callback, "SimEject");
    EXPECT_EQ(calls[0].command.action, RemoteInputAction::PressAndRelease);
    EXPECT_EQ(calls[0].function, FindFunctionFromString("SimEject"));
    EXPECT_EQ(calls[1].command.callback, "SimPickle");
    EXPECT_EQ(calls[1].command.action, RemoteInputAction::Press);
    EXPECT_EQ(calls[2].command.callback, "SimPickle");
    EXPECT_EQ(calls[2].command.action, RemoteInputAction::Release);
    for (const RemoteInputCall& call : calls)
    {
        EXPECT_NE(call.function, nullptr);
    }

    const std::vector<std::string> printed = TakeRecordedMonoPrint();
    EXPECT_TRUE(WasPrinted(printed, "RemoteInput [info] Listening on 127.0.0.1:"));
    EXPECT_TRUE(WasPrinted(printed, "asked for NoSuchCallback, but there is no callback of that name"));
}

TEST_F(RemoteInputGameTest, ReportsEachUnknownNameOnce)
{
    ASSERT_TRUE(StartRemoteInput()) << "see the log above";

    TestClient client;
    client.Send("NoSuchCallback\nOtherMissingCallback\nNoSuchCallback\nSimEject\n");
    ASSERT_EQ(GetCallsUntil(1).size(), 1u);

    const std::vector<std::string> printed = TakeRecordedMonoPrint();
    EXPECT_EQ(CountPrinted(printed, "RemoteInput [warning] Client "), 2u);
    EXPECT_EQ(CountPrinted(printed, "asked for NoSuchCallback, but"), 1u);
    EXPECT_EQ(CountPrinted(printed, "asked for OtherMissingCallback, but"), 1u);
}

TEST_F(RemoteInputGameTest, StopsReportingUnknownNamesAfterSixtyFour)
{
    ASSERT_TRUE(StartRemoteInput()) << "see the log above";

    // Two clients, as one may only send so many commands at once.
    constexpr int names_per_client = 33;
    std::string first_lines;
    std::string second_lines;
    for (int name = 0; name < names_per_client; ++name)
    {
        first_lines += std::format("Missing{}\n", name);
        second_lines += std::format("Missing{}\n", names_per_client + name);
    }
    TestClient first;
    TestClient second;
    first.Send(first_lines + "SimEject\n");
    second.Send(second_lines + "SimPickle\n");
    ASSERT_EQ(GetCallsUntil(2).size(), 2u);

    const std::vector<std::string> printed = TakeRecordedMonoPrint();
    EXPECT_EQ(CountPrinted(printed, "but there is no callback of that name"), 64u);
    EXPECT_EQ(CountPrinted(printed, "Clients have asked for 64 different callbacks that do not exist"), 1u);
}

TEST_F(RemoteInputGameTest, ReportsUnknownNamesAfreshEachTimeItStarts)
{
    for (int run = 0; run < 2; ++run)
    {
        g_nCallbackSocketPortNum = FindFreePort();   // so that the second run does not depend on reusing the port
        ASSERT_TRUE(StartRemoteInput()) << "see the log above";
        TestClient client;
        client.Send("NoSuchCallback\nSimEject\n");
        ASSERT_EQ(GetCallsUntil(1).size(), 1u);
        StopRemoteInput();
    }

    EXPECT_EQ(CountPrinted(TakeRecordedMonoPrint(), "asked for NoSuchCallback, but"), 2u);
}

TEST_F(RemoteInputGameTest, ReportsAnInvalidConfig)
{
    g_nCallbackSocketMode = 7;

    EXPECT_FALSE(StartRemoteInput());
    EXPECT_TRUE(WasPrinted(TakeRecordedMonoPrint(), "RemoteInput [error] Mode 7 is not valid"));
}
