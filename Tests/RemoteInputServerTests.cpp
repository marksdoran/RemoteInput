// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: http://www.viva64.com

// End-to-end tests: a real RemoteInputServer on a loopback port, with real TCP clients.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "ExtLibs/asio/include/asio.hpp"

#include "RemoteInputAccessPolicy.h"
#include "RemoteInputLog.h"
#include "RemoteInputServer.h"
#include "TestLog.h"

using namespace std::chrono_literals;

namespace
{
using Strings = std::vector<std::string>;

constexpr std::chrono::milliseconds wait_timeout = 5s;

// Long enough for a host name lookup to fail even where name resolution is slow, e.g. with no DNS server reachable.
constexpr std::chrono::milliseconds lookup_timeout = 30s;

RemoteInputSettings LocalHostSettings()
{
    RemoteInputSettings settings;
    settings.mode = RemoteInputMode::LocalHostOnly;
    settings.port = 0;   // any free port
    return settings;
}

RemoteInputSettings LanSettings(std::string allowed_clients)
{
    RemoteInputSettings settings;
    settings.mode = RemoteInputMode::Lan;
    settings.port = 0;
    settings.allowed_clients = std::move(allowed_clients);
    return settings;
}

// The address this computer would send from to reach `destination`, unless that is a loopback or link-local
// address. A connection to that address looks to the server like one from another computer. Connecting a UDP
// socket sends nothing; it only makes the OS choose the local address.
std::optional<asio::ip::address> OwnAddressTowards(std::string_view destination)
{
    asio::error_code error;
    const asio::ip::address destination_address = asio::ip::make_address(destination, error);
    asio::io_context io_context;
    asio::ip::udp::socket socket(io_context);
    socket.open(destination_address.is_v6() ? asio::ip::udp::v6() : asio::ip::udp::v4(), error);
    socket.connect(asio::ip::udp::endpoint(destination_address, 9), error);
    asio::ip::address address = NormalizeAddress(socket.local_endpoint(error).address());
    const bool is_link_local = address.is_v6() && address.to_v6().is_link_local();   // would need a scope id
    if (error || address.is_loopback() || address.is_unspecified() || is_link_local)
    {
        return std::nullopt;
    }
    return address;
}

// One of this computer's local network addresses, if it has any.
std::optional<asio::ip::address> FindOwnLanAddress()
{
    // One destination in each local network range, so that whichever one this computer's network uses is found.
    for (const std::string_view destination : { "192.168.255.254", "10.255.255.254", "172.31.255.254", "fdff::1" })
    {
        const std::optional<asio::ip::address> address = OwnAddressTowards(destination);
        if (address && IsLanAddress(*address))
        {
            return address;
        }
    }
    return std::nullopt;
}

// One of this computer's addresses that is not on a local network, such as a public IPv6 address, if it has any.
std::optional<asio::ip::address> FindOwnNonLanAddress()
{
    // Both destinations are reserved for documentation, so they are never real hosts.
    for (const std::string_view destination : { "192.0.2.1", "2001:db8::1" })
    {
        const std::optional<asio::ip::address> address = OwnAddressTowards(destination);
        if (address && !IsLanAddress(*address))
        {
            return address;
        }
    }
    return std::nullopt;
}

// Any of this computer's addresses other than loopback.
std::optional<asio::ip::address> FindOwnAddress()
{
    const std::optional<asio::ip::address> lan_address = FindOwnLanAddress();
    return lan_address ? lan_address : FindOwnNonLanAddress();
}

// The local network addresses that `name` resolves to, leaving out link-local IPv6 addresses (they would need a
// scope id to connect to).
std::vector<asio::ip::address> LookUpLanAddresses(const std::string& name)
{
    asio::io_context io_context;
    asio::ip::tcp::resolver resolver(io_context);
    asio::error_code error;
    const asio::ip::tcp::resolver::results_type results = resolver.resolve(name, "0", error);

    std::vector<asio::ip::address> addresses;
    for (const auto& entry : results)
    {
        const asio::ip::address address = NormalizeAddress(entry.endpoint().address());
        if (IsLanAddress(address) && !(address.is_v6() && address.to_v6().is_link_local()))
        {
            addresses.push_back(address);
        }
    }
    return addresses;
}

// An address as the server's log shows it at the start of a client's name: "192.168.1.20" or "[fd00::20]".
std::string AsLogged(const asio::ip::address& address)
{
    return address.is_v6() ? std::format("[{}]", address.to_string()) : address.to_string();
}

// Stands in for a companion app: a plain blocking TCP client.
class TestClient final
{
public:
    explicit TestClient(std::uint16_t port, const asio::ip::address& address = asio::ip::address_v4::loopback()) :
        socket_(io_context_)
    {
        asio::error_code error;
        socket_.connect(asio::ip::tcp::endpoint(address, port), error);
        EXPECT_FALSE(error) << "connect failed: " << error.message();
    }

    void Send(std::string_view data)
    {
        asio::error_code error;
        asio::write(socket_, asio::buffer(data), error);
        EXPECT_FALSE(error) << "send failed: " << error.message();
    }

    // Returns true once the server has closed the connection, or false if it is still open at the timeout. The
    // server never sends anything, so a read that does not have to wait means the connection is over.
    bool WaitForServerToClose(std::chrono::milliseconds timeout = wait_timeout)
    {
        asio::error_code error;
        socket_.non_blocking(true, error);
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline)
        {
            char byte = 0;
            socket_.read_some(asio::buffer(&byte, 1), error);
            if (error != asio::error::would_block)
            {
                return true;
            }
            std::this_thread::sleep_for(1ms);
        }
        return false;
    }

    void Close()
    {
        asio::error_code ignored;
        socket_.close(ignored);
    }

    // Closes the connection abortively: the server sees it reset rather than closed, as when a client crashes.
    void Reset()
    {
        asio::error_code ignored;
        socket_.set_option(asio::socket_base::linger(true, 0), ignored);
        socket_.close(ignored);
    }

private:
    asio::io_context io_context_;
    asio::ip::tcp::socket socket_;
};

// Drains the server the way the game does, once per "frame", until `count` commands have arrived or the timeout
// has passed.
std::vector<RemoteInputCommand> DrainUntil(RemoteInputServer& server, std::size_t count,
    std::chrono::milliseconds timeout = wait_timeout)
{
    std::vector<RemoteInputCommand> received;
    std::vector<RemoteInputCommand> frame;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (received.size() < count && std::chrono::steady_clock::now() < deadline)
    {
        server.DrainRemoteInput(frame);
        received.insert(received.end(), std::make_move_iterator(frame.begin()), std::make_move_iterator(frame.end()));
        std::this_thread::sleep_for(1ms);
    }
    return received;
}

// Each command as the client would have sent it, e.g. "SimEject_Press".
Strings Describe(const std::vector<RemoteInputCommand>& commands)
{
    Strings descriptions;
    descriptions.reserve(commands.size());
    for (const RemoteInputCommand& command : commands)
    {
        descriptions.push_back(MakeRemoteInputCommand(command.callback, command.action));
    }
    return descriptions;
}
} // namespace

TEST(RemoteInputServer, DoesNotListenWhenDisabled)
{
    TestLog log;
    RemoteInputServer server(log);

    EXPECT_FALSE(server.Start(RemoteInputSettings{}));
    EXPECT_FALSE(server.IsRunning());
    EXPECT_EQ(server.Port(), 0);
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Info, "Disabled")) << log.Dump();
}

TEST(RemoteInputServer, GivesNothingBeforeStart)
{
    TestLog log;
    RemoteInputServer server(log);

    std::vector<RemoteInputCommand> commands(2);
    server.DrainRemoteInput(commands);
    EXPECT_TRUE(commands.empty());
}

TEST(RemoteInputServer, ReceivesCommandsInOrder)
{
    TestLog log;
    RemoteInputServer server(log);
    ASSERT_TRUE(server.Start(LocalHostSettings())) << log.Dump();
    EXPECT_TRUE(server.IsRunning());
    EXPECT_NE(server.Port(), 0);

    TestClient client(server.Port());
    client.Send("SimEject\nSimEject_Press\nSimEject_Release\n");

    const std::vector<RemoteInputCommand> commands = DrainUntil(server, 3);
    ASSERT_EQ(Describe(commands), (Strings{ "SimEject", "SimEject_Press", "SimEject_Release" })) << log.Dump();
    EXPECT_EQ(commands[0].action, RemoteInputAction::PressAndRelease);
    EXPECT_EQ(commands[1].action, RemoteInputAction::Press);
    EXPECT_EQ(commands[2].action, RemoteInputAction::Release);
    for (const RemoteInputCommand& command : commands)
    {
        EXPECT_EQ(command.callback, "SimEject");
        EXPECT_EQ(command.client_id, commands.front().client_id);
    }

    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Info, "Listening on 127.0.0.1:")) << log.Dump();
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Info, "(local host only)")) << log.Dump();
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Info, "connected from 127.0.0.1:")) << log.Dump();
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Verbose, ": SimEject_Press")) << log.Dump();
}

TEST(RemoteInputServer, AcceptsCrLfLineEndings)
{
    TestLog log;
    RemoteInputServer server(log);
    ASSERT_TRUE(server.Start(LocalHostSettings())) << log.Dump();

    TestClient client(server.Port());
    client.Send("SimEject\r\nSimPickle\r\n");

    EXPECT_EQ(Describe(DrainUntil(server, 2)), (Strings{ "SimEject", "SimPickle" })) << log.Dump();
}

TEST(RemoteInputServer, ReassemblesLinesSplitAcrossWrites)
{
    TestLog log;
    RemoteInputServer server(log);
    ASSERT_TRUE(server.Start(LocalHostSettings())) << log.Dump();

    TestClient client(server.Port());
    client.Send("Sim");
    std::this_thread::sleep_for(20ms);
    client.Send("Eject\nSimPi");
    std::this_thread::sleep_for(20ms);
    client.Send("ckle\n");

    EXPECT_EQ(Describe(DrainUntil(server, 2)), (Strings{ "SimEject", "SimPickle" })) << log.Dump();
}

TEST(RemoteInputServer, IgnoresEmptyLinesAndLogsInvalidOnes)
{
    TestLog log;
    RemoteInputServer server(log);
    ASSERT_TRUE(server.Start(LocalHostSettings())) << log.Dump();

    TestClient client(server.Port());
    client.Send("\n\r\nSim Eject\nbad-name\nOK_1\n");

    // Lines are handled in order, so once OK_1 has arrived the lines before it have been dealt with.
    EXPECT_EQ(Describe(DrainUntil(server, 1)), Strings{ "OK_1" }) << log.Dump();
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Warning, "ignored \"Sim Eject\"")) << log.Dump();
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Warning, "ignored \"bad-name\"")) << log.Dump();
    EXPECT_EQ(log.Count(RemoteInputLogLevel::Warning), 2u) << log.Dump();
}

TEST(RemoteInputServer, IgnoresAByteOrderMarkAtTheStartOfAConnection)
{
    TestLog log;
    RemoteInputServer server(log);
    ASSERT_TRUE(server.Start(LocalHostSettings())) << log.Dump();

    // Some text tools start what they send with one. Anywhere else it is an invalid character.
    TestClient client(server.Port());
    client.Send("\xEF\xBB\xBFSimEject\n\xEF\xBB\xBFSimPickle\nMarker\n");

    EXPECT_EQ(Describe(DrainUntil(server, 2)), (Strings{ "SimEject", "Marker" })) << log.Dump();
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Warning, "ignored \"\\xEF\\xBB\\xBFSimPickle\"")) << log.Dump();
}

TEST(RemoteInputServer, DisconnectsClientThatSendsTooLongALine)
{
    TestLog log;
    RemoteInputLimits limits;
    limits.max_line_length = 16;
    RemoteInputServer server(log, limits);
    ASSERT_TRUE(server.Start(LocalHostSettings())) << log.Dump();

    TestClient client(server.Port());
    client.Send(std::string(16, 'A') + "\n");     // exactly at the limit
    client.Send(std::string(15, 'B') + "\r\n");   // at the limit, counting the CR
    client.Send(std::string(17, 'C') + "\n");     // one byte over

    EXPECT_TRUE(client.WaitForServerToClose());
    const Strings expected{ std::string(16, 'A'), std::string(15, 'B') };
    EXPECT_EQ(Describe(DrainUntil(server, 2)), expected) << log.Dump();
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Warning, "longer than 16 bytes, starting \"CCCC")) << log.Dump();
}

TEST(RemoteInputServer, DiscardsUnfinishedLineWhenClientDisconnects)
{
    TestLog log;
    RemoteInputServer server(log);
    ASSERT_TRUE(server.Start(LocalHostSettings())) << log.Dump();

    TestClient client(server.Port());
    client.Send("SimEject\nSimEject_Rel");
    client.Close();

    ASSERT_TRUE(log.WaitFor(RemoteInputLogLevel::Warning, "unfinished line \"SimEject_Rel\" was discarded"))
        << log.Dump();
    EXPECT_EQ(Describe(DrainUntil(server, 1)), Strings{ "SimEject" }) << log.Dump();
}

TEST(RemoteInputServer, ReleasesHeldInputsWhenAClientDisconnects)
{
    TestLog log;
    RemoteInputServer server(log);
    ASSERT_TRUE(server.Start(LocalHostSettings())) << log.Dump();

    TestClient client(server.Port());
    client.Send("SimTriggerFirstDetent_Press\nSimPickle_Press\nSimPickle_Release\nSimEject\n");
    client.Close();

    // SimPickle was released by the client itself, so only the trigger is released for it.
    const std::vector<RemoteInputCommand> commands = DrainUntil(server, 5);
    EXPECT_EQ(Describe(commands), (Strings{ "SimTriggerFirstDetent_Press", "SimPickle_Press", "SimPickle_Release",
        "SimEject", "SimTriggerFirstDetent_Release" })) << log.Dump();
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Info,
        "releasing what the client left pressed: SimTriggerFirstDetent")) << log.Dump();
}

TEST(RemoteInputServer, ReleasesHeldInputsWhenAConnectionIsLost)
{
    TestLog log;
    RemoteInputServer server(log);
    ASSERT_TRUE(server.Start(LocalHostSettings())) << log.Dump();

    TestClient client(server.Port());
    client.Send("SimPickle_Press\n");
    ASSERT_EQ(Describe(DrainUntil(server, 1)), Strings{ "SimPickle_Press" }) << log.Dump();
    client.Reset();

    EXPECT_EQ(Describe(DrainUntil(server, 1)), Strings{ "SimPickle_Release" }) << log.Dump();
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Info, ": connection lost: ")) << log.Dump();
}

TEST(RemoteInputServer, LimitsHowManyInputsAClientCanHold)
{
    TestLog log;
    RemoteInputLimits limits;
    limits.max_held_inputs = 2;
    RemoteInputServer server(log, limits);
    ASSERT_TRUE(server.Start(LocalHostSettings())) << log.Dump();

    TestClient client(server.Port());
    client.Send("A_Press\nB_Press\nC_Press\nA_Press\nA_Release\nC_Press\n");

    // C cannot be pressed while A and B are held; pressing A again is not a new hold; once A is released, C can be.
    EXPECT_EQ(Describe(DrainUntil(server, 5)), (Strings{ "A_Press", "B_Press", "A_Press", "A_Release", "C_Press" }))
        << log.Dump();
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Warning,
        "dropped C_Press: each connection can hold at most 2 inputs")) << log.Dump();
}

TEST(RemoteInputServer, SlowsDownAClientThatSendsTooFast)
{
    TestLog log;
    RemoteInputLimits limits;
    limits.max_commands_per_second = 5.0;   // also the most a client can send at once
    RemoteInputServer server(log, limits);
    ASSERT_TRUE(server.Start(LocalHostSettings())) << log.Dump();

    // The lines arrive together, far faster than five a second. The first five are handled at once; F and G each
    // wait a fifth of a second for the allowance. The release of the held A is never held back, but it cannot
    // overtake F, which came first. Nothing is dropped.
    TestClient client(server.Port());
    const auto sent_at = std::chrono::steady_clock::now();
    client.Send("A_Press\nB\nC\nD\nE\nF\nA_Release\nG\n");

    EXPECT_EQ(Describe(DrainUntil(server, 8)), (Strings{ "A_Press", "B", "C", "D", "E", "F", "A_Release", "G" }))
        << log.Dump();
    EXPECT_GE(std::chrono::steady_clock::now() - sent_at, 300ms);
    EXPECT_EQ(log.Count(RemoteInputLogLevel::Warning, "slowing its commands down to 5 a second"), 1u) << log.Dump();
}

TEST(RemoteInputServer, ReleasesGetThroughAFullQueue)
{
    TestLog log;
    RemoteInputLimits limits;
    limits.max_queued_commands = 2;
    RemoteInputServer server(log, limits);
    ASSERT_TRUE(server.Start(LocalHostSettings())) << log.Dump();

    TestClient client(server.Port());
    client.Send("A_Press\nB\nC\nA_Release\nD_Release\nMarker\n");
    ASSERT_TRUE(log.WaitFor(RemoteInputLogLevel::Warning, "dropped Marker")) << log.Dump();

    // C, D_Release (nothing was holding D) and Marker were dropped; A_Release was not, as A was held.
    std::vector<RemoteInputCommand> commands;
    server.DrainRemoteInput(commands);
    EXPECT_EQ(Describe(commands), (Strings{ "A_Press", "B", "A_Release" })) << log.Dump();
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Warning, "dropped C")) << log.Dump();
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Warning, "dropped D_Release")) << log.Dump();
}

TEST(RemoteInputServer, GivesEachClientItsOwnId)
{
    TestLog log;
    RemoteInputServer server(log);
    ASSERT_TRUE(server.Start(LocalHostSettings())) << log.Dump();

    TestClient first(server.Port());
    first.Send("One\n");
    const std::vector<RemoteInputCommand> first_commands = DrainUntil(server, 1);
    TestClient second(server.Port());
    second.Send("Two\n");
    const std::vector<RemoteInputCommand> second_commands = DrainUntil(server, 1);

    ASSERT_EQ(Describe(first_commands), Strings{ "One" }) << log.Dump();
    ASSERT_EQ(Describe(second_commands), Strings{ "Two" }) << log.Dump();
    EXPECT_NE(first_commands[0].client_id, second_commands[0].client_id);
}

TEST(RemoteInputServer, NeverReusesClientIdsAfterARestart)
{
    TestLog log;
    RemoteInputServer server(log);
    ASSERT_TRUE(server.Start(LocalHostSettings())) << log.Dump();
    TestClient first(server.Port());
    first.Send("One\n");
    const std::vector<RemoteInputCommand> first_commands = DrainUntil(server, 1);
    server.Stop();

    // A command from before the restart can still be waiting in the game's hands, so the ids must not collide.
    ASSERT_TRUE(server.Start(LocalHostSettings())) << log.Dump();
    TestClient second(server.Port());
    second.Send("Two\n");
    const std::vector<RemoteInputCommand> second_commands = DrainUntil(server, 1);

    ASSERT_EQ(Describe(first_commands), Strings{ "One" }) << log.Dump();
    ASSERT_EQ(Describe(second_commands), Strings{ "Two" }) << log.Dump();
    EXPECT_GT(second_commands[0].client_id, first_commands[0].client_id);
}

TEST(RemoteInputServer, HandlesManyClientsAtOnce)
{
    constexpr std::size_t client_count = 8;
    constexpr std::size_t commands_per_client = 100;

    TestLog log;
    RemoteInputLimits limits;
    limits.max_commands_per_second = 1e6;   // this test is about concurrency, not rate limits
    RemoteInputServer server(log, limits);
    ASSERT_TRUE(server.Start(LocalHostSettings())) << log.Dump();
    const std::uint16_t port = server.Port();

    {
        std::vector<std::jthread> clients;
        for (std::size_t client_number = 0; client_number < client_count; ++client_number)
        {
            clients.emplace_back([port, client_number]
                {
                    TestClient client(port);
                    for (std::size_t sequence = 0; sequence < commands_per_client; ++sequence)
                    {
                        client.Send(std::format("C{}_{}\n", client_number, sequence));
                    }
                    client.Close();
                });
        }
    }

    const std::vector<RemoteInputCommand> commands = DrainUntil(server, client_count * commands_per_client);

    // Commands from different clients interleave, but each client's commands must arrive complete and in order.
    std::map<RemoteInputClientId, Strings> commands_by_client;
    for (const RemoteInputCommand& command : commands)
    {
        commands_by_client[command.client_id].push_back(command.callback);
    }
    ASSERT_EQ(commands_by_client.size(), client_count) << log.Dump();
    for (const auto& [client_id, client_commands] : commands_by_client)
    {
        ASSERT_EQ(client_commands.size(), commands_per_client) << "client " << client_id;
        const std::string prefix = client_commands.front().substr(0, client_commands.front().find('_') + 1);
        for (std::size_t sequence = 0; sequence < client_commands.size(); ++sequence)
        {
            EXPECT_EQ(client_commands[sequence], std::format("{}{}", prefix, sequence)) << "client " << client_id;
        }
    }
}

TEST(RemoteInputServer, RefusesConnectionsBeyondTheLimit)
{
    TestLog log;
    RemoteInputLimits limits;
    limits.max_connections = 2;
    limits.max_new_connections_per_second = 1000.0;   // so that every client can send a command at once
    RemoteInputServer server(log, limits);
    ASSERT_TRUE(server.Start(LocalHostSettings())) << log.Dump();

    TestClient first(server.Port());
    first.Send("One\n");
    TestClient second(server.Port());
    second.Send("Two\n");
    ASSERT_EQ(DrainUntil(server, 2).size(), 2u) << log.Dump();

    TestClient third(server.Port());
    EXPECT_TRUE(third.WaitForServerToClose());
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Warning, "too many clients are connected (the limit is 2)"))
        << log.Dump();

    // Once a client leaves, its slot can be used again.
    first.Close();
    ASSERT_TRUE(log.WaitFor(RemoteInputLogLevel::Info, ": disconnected")) << log.Dump();
    TestClient fourth(server.Port());
    fourth.Send("Four\n");
    EXPECT_EQ(Describe(DrainUntil(server, 1)), Strings{ "Four" }) << log.Dump();
}

TEST(RemoteInputServer, ClientsThatConnectTooOftenStartWithNoCommandAllowance)
{
    TestLog log;
    RemoteInputLimits limits;
    limits.max_connections = 1;                    // also how many connections get a burst of commands at first
    limits.max_new_connections_per_second = 0.01;
    limits.max_commands_per_second = 2.0;
    RemoteInputServer server(log, limits);
    ASSERT_TRUE(server.Start(LocalHostSettings())) << log.Dump();

    TestClient first(server.Port());
    first.Send("One\n");
    ASSERT_EQ(Describe(DrainUntil(server, 1)), Strings{ "One" }) << log.Dump();
    first.Close();
    ASSERT_TRUE(log.WaitFor(RemoteInputLogLevel::Info, ": disconnected")) << log.Dump();

    // The next connection comes too soon to get a burst, so it is let in, but its command waits for the allowance
    // to build up: a client reconnecting in a loop gains nothing, and locks nobody out.
    TestClient second(server.Port());
    const auto sent_at = std::chrono::steady_clock::now();
    second.Send("Two\n");

    EXPECT_EQ(Describe(DrainUntil(server, 1)), Strings{ "Two" }) << log.Dump();
    EXPECT_GE(std::chrono::steady_clock::now() - sent_at, 400ms);   // the allowance takes half a second
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Info, "clients are connecting very often, so its commands"))
        << log.Dump();
    EXPECT_EQ(log.Count(RemoteInputLogLevel::Warning), 0u) << log.Dump();
}

TEST(RemoteInputServer, StopEndsAClientThatIsBeingSlowedDown)
{
    TestLog log;
    RemoteInputLimits limits;
    limits.max_commands_per_second = 0.01;   // one command, then nothing for a hundred seconds
    RemoteInputServer server(log, limits);
    ASSERT_TRUE(server.Start(LocalHostSettings())) << log.Dump();

    TestClient client(server.Port());
    client.Send("SimPickle_Press\nSimEject\n");
    ASSERT_EQ(Describe(DrainUntil(server, 1)), Strings{ "SimPickle_Press" }) << log.Dump();
    ASSERT_TRUE(log.WaitFor(RemoteInputLogLevel::Warning, "slowing its commands down")) << log.Dump();

    const auto stop_started = std::chrono::steady_clock::now();
    server.Stop();
    EXPECT_LT(std::chrono::steady_clock::now() - stop_started, 2s);
    EXPECT_TRUE(client.WaitForServerToClose());

    // The waiting SimEject is never handled, but the held SimPickle is still released.
    std::vector<RemoteInputCommand> commands;
    server.DrainRemoteInput(commands);
    EXPECT_EQ(Describe(commands), Strings{ "SimPickle_Release" }) << log.Dump();
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Info, "disconnected by the server")) << log.Dump();
}

TEST(RemoteInputServer, KeepsMessagesAboutClientsFromFloodingTheLog)
{
    constexpr std::size_t refused_count = 40;

    TestLog log;
    RemoteInputLimits limits;
    limits.max_connections = 1;
    limits.max_new_connections_per_second = 1000.0;
    RemoteInputServer server(log, limits);
    ASSERT_TRUE(server.Start(LocalHostSettings())) << log.Dump();

    TestClient first(server.Port());
    first.Send("One\n");
    ASSERT_EQ(Describe(DrainUntil(server, 1)), Strings{ "One" }) << log.Dump();
    for (std::size_t attempt = 0; attempt < refused_count; ++attempt)
    {
        TestClient refused(server.Port());
        ASSERT_TRUE(refused.WaitForServerToClose());
    }

    // About twenty get through at once, then about one a second; the rest are counted, and the count is logged when
    // the server stops.
    const std::size_t logged = log.Count(RemoteInputLogLevel::Warning, "Refused connection");
    EXPECT_GE(logged, 10u) << log.Dump();
    EXPECT_LE(logged, 25u) << log.Dump();

    server.Stop();
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Info, "about clients were not logged, to keep the log readable"))
        << log.Dump();
}

TEST(RemoteInputServer, DropsCommandsWhenTooManyAreWaiting)
{
    TestLog log;
    RemoteInputLimits limits;
    limits.max_queued_commands = 3;
    RemoteInputServer server(log, limits);
    ASSERT_TRUE(server.Start(LocalHostSettings())) << log.Dump();

    TestClient client(server.Port());
    client.Send("A1\nA2\nA3\nA4\nA5\n");
    ASSERT_TRUE(log.WaitFor(RemoteInputLogLevel::Warning, "dropped A5")) << log.Dump();
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Warning, "dropped A4")) << log.Dump();

    std::vector<RemoteInputCommand> commands;
    server.DrainRemoteInput(commands);
    EXPECT_EQ(Describe(commands), (Strings{ "A1", "A2", "A3" }));

    // Draining made room again.
    client.Send("A6\n");
    EXPECT_EQ(Describe(DrainUntil(server, 1)), Strings{ "A6" }) << log.Dump();
}

TEST(RemoteInputServer, StopDisconnectsClientsAndReleasesWhatTheyHeld)
{
    TestLog log;
    RemoteInputServer server(log);
    ASSERT_TRUE(server.Start(LocalHostSettings())) << log.Dump();

    TestClient client(server.Port());
    client.Send("SimTriggerFirstDetent_Press\n");
    ASSERT_EQ(Describe(DrainUntil(server, 1)), Strings{ "SimTriggerFirstDetent_Press" }) << log.Dump();

    const auto stop_started = std::chrono::steady_clock::now();
    server.Stop();
    EXPECT_LT(std::chrono::steady_clock::now() - stop_started, 2s);
    EXPECT_FALSE(server.IsRunning());
    EXPECT_EQ(server.Port(), 0);
    EXPECT_TRUE(client.WaitForServerToClose());

    // The release is still delivered after Stop(), so the game does not keep the trigger held.
    std::vector<RemoteInputCommand> commands;
    server.DrainRemoteInput(commands);
    EXPECT_EQ(Describe(commands), Strings{ "SimTriggerFirstDetent_Release" });
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Info, "disconnected by the server")) << log.Dump();
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Info, "Stopped")) << log.Dump();
}

TEST(RemoteInputServer, StopIsHarmlessWhenNotRunning)
{
    TestLog log;
    RemoteInputServer server(log);
    server.Stop();

    ASSERT_TRUE(server.Start(LocalHostSettings())) << log.Dump();
    server.Stop();
    server.Stop();
    EXPECT_FALSE(server.IsRunning());
}

TEST(RemoteInputServer, CanStartAgainAfterStopping)
{
    TestLog log;
    RemoteInputServer server(log);
    ASSERT_TRUE(server.Start(LocalHostSettings())) << log.Dump();
    server.Stop();
    ASSERT_TRUE(server.Start(LocalHostSettings())) << log.Dump();

    TestClient client(server.Port());
    client.Send("SimEject\n");
    EXPECT_EQ(Describe(DrainUntil(server, 1)), Strings{ "SimEject" }) << log.Dump();
}

TEST(RemoteInputServer, FailsToStartWhenThePortIsTaken)
{
    TestLog first_log;
    RemoteInputServer first(first_log);
    ASSERT_TRUE(first.Start(LocalHostSettings())) << first_log.Dump();

    TestLog second_log;
    RemoteInputServer second(second_log);
    RemoteInputSettings settings = LocalHostSettings();
    settings.port = first.Port();

    EXPECT_FALSE(second.Start(settings));
    EXPECT_FALSE(second.IsRunning());
    EXPECT_TRUE(second_log.Contains(RemoteInputLogLevel::Error, "could not listen")) << second_log.Dump();
}

TEST(RemoteInputServer, AcceptsIpv6LoopbackClients)
{
    asio::io_context probe_context;
    asio::ip::tcp::socket probe(probe_context);
    asio::error_code probe_error;
    probe.open(asio::ip::tcp::v6(), probe_error);
    if (probe_error)
    {
        GTEST_SKIP() << "IPv6 is not available on this computer";
    }

    TestLog log;
    RemoteInputServer server(log);
    ASSERT_TRUE(server.Start(LocalHostSettings())) << log.Dump();

    TestClient client(server.Port(), asio::ip::address_v6::loopback());
    client.Send("SimEject\n");

    EXPECT_EQ(Describe(DrainUntil(server, 1)), Strings{ "SimEject" }) << log.Dump();
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Info, "connected from [::1]:")) << log.Dump();
}

// The LAN mode tests listen on every interface, so on Windows the firewall may ask whether the test program may
// accept connections. Either answer is fine: every connection stays on this computer. Tests that need this computer
// to have a particular kind of address skip themselves when it has none.

TEST(RemoteInputServer, LanModeListensOnLoopbackOnlyWhenNoLanClientIsAllowed)
{
    TestLog log;
    RemoteInputServer server(log);
    ASSERT_TRUE(server.Start(LanSettings("8.8.8.8"))) << log.Dump();

    TestClient client(server.Port());
    client.Send("SimEject\n");

    EXPECT_EQ(Describe(DrainUntil(server, 1)), Strings{ "SimEject" }) << log.Dump();
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Info, "Listening on 127.0.0.1:")) << log.Dump();
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Info, "(LAN, but no LAN client is allowed)")) << log.Dump();
}

TEST(RemoteInputServer, LanModeDoesNotLookUpHostNamesUntilAClientNeedsIt)
{
    TestLog log;
    RemoteInputServer server(log);
    ASSERT_TRUE(server.Start(LanSettings("no-such-host.invalid, 10.123.45.67"))) << log.Dump();

    // Programs on this computer are always allowed, so they never need the names looked up.
    TestClient client(server.Port());
    client.Send("SimEject\n");
    EXPECT_EQ(Describe(DrainUntil(server, 1)), Strings{ "SimEject" }) << log.Dump();
    server.Stop();

    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Info, "Listening on 0.0.0.0:")) << log.Dump();
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Info, "(LAN)")) << log.Dump();
    EXPECT_FALSE(log.Contains(RemoteInputLogLevel::Info, "Looking up")) << log.Dump();
    EXPECT_FALSE(log.Contains(RemoteInputLogLevel::Warning, "could not be looked up")) << log.Dump();
}

TEST(RemoteInputServer, LanModeRefusesClientsThatAreNotAllowed)
{
    const std::optional<asio::ip::address> own_address = FindOwnAddress();
    if (!own_address)
    {
        GTEST_SKIP() << "this computer has no address other than loopback";
    }

    TestLog log;
    RemoteInputServer server(log);
    ASSERT_TRUE(server.Start(LanSettings("10.123.45.67"))) << log.Dump();

    TestClient client(server.Port(), *own_address);
    EXPECT_TRUE(client.WaitForServerToClose());
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Warning,
        std::format("Refused connection from {}:", AsLogged(*own_address)))) << log.Dump();
}

TEST(RemoteInputServer, LanModeHoldsUnknownClientsUntilHostNamesAreLookedUp)
{
    const std::optional<asio::ip::address> own_address = FindOwnLanAddress();
    if (!own_address)
    {
        GTEST_SKIP() << "this computer has no local network address";
    }

    // .invalid is reserved: no name under it ever resolves.
    TestLog log;
    RemoteInputServer server(log);
    ASSERT_TRUE(server.Start(LanSettings("no-such-host.invalid"))) << log.Dump();

    TestClient client(server.Port(), *own_address);
    EXPECT_TRUE(client.WaitForServerToClose(lookup_timeout));
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Info, std::format(
        "Looking up the allowed host names before deciding whether {}:", AsLogged(*own_address)))) << log.Dump();
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Warning, "\"no-such-host.invalid\" could not be looked up"))
        << log.Dump();
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Warning,
        std::format("Refused connection from {}:", AsLogged(*own_address)))) << log.Dump();
}

TEST(RemoteInputServer, LanModeLimitsHowManyClientsWaitForALookup)
{
    const std::optional<asio::ip::address> own_address = FindOwnLanAddress();
    if (!own_address)
    {
        GTEST_SKIP() << "this computer has no local network address";
    }

    TestLog log;
    RemoteInputLimits limits;
    limits.max_connections = 1;                 // also how many clients can wait for a lookup
    limits.host_lookup_interval = std::chrono::hours(1);
    RemoteInputServer server(log, limits);
    ASSERT_TRUE(server.Start(LanSettings("no-such-host.invalid"))) << log.Dump();

    // The first client's lookup fails, so it is refused. The second comes too soon for another lookup, so it
    // waits for one; that leaves no room for the third.
    TestClient first(server.Port(), *own_address);
    ASSERT_TRUE(first.WaitForServerToClose(lookup_timeout)) << log.Dump();
    TestClient second(server.Port(), *own_address);
    ASSERT_TRUE(log.WaitFor(RemoteInputLogLevel::Info, "Looking up the allowed host names", 2)) << log.Dump();
    TestClient third(server.Port(), *own_address);
    EXPECT_TRUE(third.WaitForServerToClose());
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Warning,
        "too many clients are waiting for the allowed host names to be looked up")) << log.Dump();

    // Stopping does not wait for the lookup that the second client is waiting for.
    const auto stop_started = std::chrono::steady_clock::now();
    server.Stop();
    EXPECT_LT(std::chrono::steady_clock::now() - stop_started, 2s);
    EXPECT_TRUE(second.WaitForServerToClose());
}

TEST(RemoteInputServer, LanModeAcceptsAllowedAddresses)
{
    const std::optional<asio::ip::address> own_address = FindOwnLanAddress();
    if (!own_address)
    {
        GTEST_SKIP() << "this computer has no local network address";
    }

    TestLog log;
    RemoteInputServer server(log);
    ASSERT_TRUE(server.Start(LanSettings(own_address->to_string()))) << log.Dump();

    TestClient client(server.Port(), *own_address);
    client.Send("SimEject\n");
    EXPECT_EQ(Describe(DrainUntil(server, 1)), Strings{ "SimEject" }) << log.Dump();
}

TEST(RemoteInputServer, LanModeAcceptsAllowedHostNames)
{
    // This computer's own name stands in for a phone's. On some systems it resolves only to a loopback address
    // (Debian-based ones use 127.0.1.1), which cannot show anything here.
    asio::error_code error;
    const std::string own_name = asio::ip::host_name(error);
    ASSERT_FALSE(error) << error.message();
    const std::vector<asio::ip::address> named_addresses = LookUpLanAddresses(own_name);
    if (named_addresses.empty())
    {
        GTEST_SKIP() << "this computer's name, " << own_name << ", does not resolve to a local network address";
    }

    TestLog log;
    RemoteInputServer server(log);
    ASSERT_TRUE(server.Start(LanSettings(own_name))) << log.Dump();

    // Names are only looked up when needed, so this client waits for the lookup and is then let in.
    TestClient client(server.Port(), named_addresses.front());
    client.Send("SimEject\n");
    EXPECT_EQ(Describe(DrainUntil(server, 1, lookup_timeout)), Strings{ "SimEject" }) << log.Dump();
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Info, "Looking up the allowed host names before deciding"))
        << log.Dump();
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Info, std::format("Allowed client {} resolves to ",
        QuoteForRemoteInputLog(own_name)))) << log.Dump();
}
