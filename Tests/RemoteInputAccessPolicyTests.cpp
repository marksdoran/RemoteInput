// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: http://www.viva64.com
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "gtest/gtest.h"

#include "RemoteInputAccessPolicy.h"
#include "RemoteInputLog.h"
#include "TestLog.h"

namespace
{
asio::ip::address Address(std::string_view text)
{
    asio::error_code error;
    const asio::ip::address address = asio::ip::make_address(text, error);
    EXPECT_FALSE(error) << "test address " << text << " does not parse";
    return address;
}

RemoteInputAddressRange Range(std::string_view text)
{
    const std::optional<RemoteInputAddressRange> range = RemoteInputAddressRange::Parse(text);
    EXPECT_TRUE(range.has_value()) << "test range " << text << " does not parse";
    return range.value_or(RemoteInputAddressRange(asio::ip::address_v4::any(), 32));
}

RemoteInputAccessPolicy MakePolicy(RemoteInputMode mode, std::string allowed_clients, TestLog& log)
{
    RemoteInputSettings settings;
    settings.mode = mode;
    settings.port = 4242;
    settings.allowed_clients = std::move(allowed_clients);
    return RemoteInputAccessPolicy::Create(settings, log);
}
} // namespace

TEST(RemoteInputAddressRange, ParsesSingleIpv4Address)
{
    const RemoteInputAddressRange range = Range("192.168.1.42");

    EXPECT_TRUE(range.Contains(Address("192.168.1.42")));
    EXPECT_FALSE(range.Contains(Address("192.168.1.43")));
    EXPECT_EQ(range.ToString(), "192.168.1.42");
}

TEST(RemoteInputAddressRange, ParsesIpv4CidrRange)
{
    const RemoteInputAddressRange range = Range("192.168.1.0/24");

    EXPECT_TRUE(range.Contains(Address("192.168.1.0")));
    EXPECT_TRUE(range.Contains(Address("192.168.1.255")));
    EXPECT_FALSE(range.Contains(Address("192.168.2.1")));
    EXPECT_EQ(range.ToString(), "192.168.1.0/24");
}

TEST(RemoteInputAddressRange, HandlesPrefixesThatAreNotWholeBytes)
{
    const RemoteInputAddressRange range = Range("172.16.0.0/12");

    EXPECT_TRUE(range.Contains(Address("172.16.0.0")));
    EXPECT_TRUE(range.Contains(Address("172.31.255.255")));
    EXPECT_FALSE(range.Contains(Address("172.32.0.0")));
    EXPECT_FALSE(range.Contains(Address("172.15.255.255")));
}

TEST(RemoteInputAddressRange, IgnoresHostBitsPastThePrefix)
{
    EXPECT_EQ(Range("192.168.1.42/24").ToString(), "192.168.1.0/24");
    EXPECT_EQ(Range("fd12:3456::1/16").ToString(), "fd12::/16");
}

TEST(RemoteInputAddressRange, ParsesIpv6)
{
    const RemoteInputAddressRange single = Range("fe80::1");
    EXPECT_TRUE(single.Contains(Address("fe80::1")));
    EXPECT_FALSE(single.Contains(Address("fe80::2")));

    const RemoteInputAddressRange block = Range("fd00::/8");
    EXPECT_TRUE(block.Contains(Address("fd12:3456:789a::1")));
    EXPECT_FALSE(block.Contains(Address("fe80::1")));
    EXPECT_EQ(block.ToString(), "fd00::/8");
}

TEST(RemoteInputAddressRange, IgnoresIpv6ScopeId)
{
    const RemoteInputAddressRange range = Range("fe80::1%3");

    EXPECT_TRUE(range.Contains(Address("fe80::1")));
    EXPECT_TRUE(range.Contains(Address("fe80::1%7")));
    EXPECT_EQ(range.ToString(), "fe80::1");
}

TEST(RemoteInputAddressRange, NeverMatchesTheOtherAddressFamily)
{
    EXPECT_FALSE(Range("0.0.0.0/0").Contains(Address("::1")));
    EXPECT_FALSE(Range("::/0").Contains(Address("127.0.0.1")));
}

TEST(RemoteInputAddressRange, MatchesIpv4MappedAddressesAsIpv4)
{
    EXPECT_TRUE(Range("192.168.1.0/24").Contains(Address("::ffff:192.168.1.5")));
    EXPECT_EQ(Range("::ffff:192.168.1.0/120").ToString(), "192.168.1.0/24");
    EXPECT_FALSE(RemoteInputAddressRange::Parse("::ffff:0.0.0.0/95").has_value());
}

TEST(RemoteInputAddressRange, RejectsMalformedText)
{
    for (const std::string_view text : { "", "/24", "192.168.1", "192.168.1.256", "192.168.1.0/33", "fe80::/129",
        "192.168.1.0/", "192.168.1.0/-1", "192.168.1.0/+8", "192.168.1.0/8x", "192.168.1.0/24/8", " 192.168.1.1",
        "192.168.1.1 ", "my-laptop", "localhost", "*" })
    {
        EXPECT_FALSE(RemoteInputAddressRange::Parse(text).has_value()) << '"' << text << '"';
    }
}

TEST(RemoteInputAddressRange, RejectsPortsAndBrackets)
{
    // Windows' own parser accepts these; the others do not, so they are turned away everywhere.
    for (const std::string_view text : { "192.168.1.42:4242", "192.168.1.0:4242/24", "[fe80::1]", "[fe80::1]:4242",
        "[::1]", "::ffff:192.168.1.5:4242" })
    {
        EXPECT_FALSE(RemoteInputAddressRange::Parse(text).has_value()) << '"' << text << '"';
    }

    // Colons are only a port when they come after the last dot.
    EXPECT_TRUE(RemoteInputAddressRange::Parse("fe80::1").has_value());
    EXPECT_TRUE(RemoteInputAddressRange::Parse("::ffff:192.168.1.5").has_value());
}

TEST(RemoteInputAddressRange, RejectsShorthandAddressForms)
{
    // Windows reads these as addresses (192.168.0.1, 192.168.1.1 and 192.168.8.1, among others); the other platforms
    // reject them, and so does Parse() everywhere, so that an entry never means something unexpected.
    for (const std::string_view text : { "192.168.1", "192.168.257", "3232235777", "0xC0A80101", "0xC0.168.1.1",
        "192.168.010.1", "192.168.1.01", "192.168.1.1.", ".192.168.1.1", "192.168.1.1%3", "::ffff:192.168.1",
        "::ffff:192.168.010.1", "fe80::1 2" })
    {
        EXPECT_FALSE(RemoteInputAddressRange::Parse(text).has_value()) << '"' << text << '"';
    }

    // Zeros on their own are fine, and so is a scope id after an IPv6 address.
    EXPECT_TRUE(RemoteInputAddressRange::Parse("10.0.0.0/8").has_value());
    EXPECT_TRUE(RemoteInputAddressRange::Parse("::ffff:192.168.1.5%3").has_value());
}

TEST(RemoteInputAddressRange, ContainsNarrowerRanges)
{
    const RemoteInputAddressRange wide = Range("192.168.0.0/16");
    const RemoteInputAddressRange narrow = Range("192.168.1.0/24");

    EXPECT_TRUE(wide.Contains(narrow));
    EXPECT_FALSE(narrow.Contains(wide));
    EXPECT_TRUE(narrow.Contains(narrow));
    EXPECT_FALSE(narrow.Contains(Range("192.168.2.0/24")));
}

TEST(IsLanAddress, AcceptsLocalNetworkAddresses)
{
    for (const std::string_view text : { "10.1.2.3", "172.16.0.1", "172.31.255.254", "192.168.0.1",
        "192.168.255.255", "169.254.10.10", "fd00::1", "fc00::1", "fe80::abcd", "::ffff:192.168.1.1" })
    {
        EXPECT_TRUE(IsLanAddress(Address(text))) << text;
    }
}

TEST(IsLanAddress, RejectsEverythingElse)
{
    for (const std::string_view text : { "8.8.8.8", "172.15.255.255", "172.32.0.1", "11.0.0.1", "100.64.0.1",
        "127.0.0.1", "0.0.0.0", "224.0.0.1", "255.255.255.255", "::1", "::", "2001:db8::1", "fec0::1" })
    {
        EXPECT_FALSE(IsLanAddress(Address(text))) << text;
    }
}

TEST(NormalizeAddress, UnwrapsIpv4MappedAddresses)
{
    const asio::ip::address normalized = NormalizeAddress(Address("::ffff:10.0.0.1"));

    EXPECT_TRUE(normalized.is_v4());
    EXPECT_EQ(normalized, Address("10.0.0.1"));
    EXPECT_EQ(NormalizeAddress(Address("::1")), Address("::1"));
    EXPECT_EQ(NormalizeAddress(Address("10.0.0.1")), Address("10.0.0.1"));
}

TEST(RemoteInputAccessPolicy, DisabledAllowsNobody)
{
    TestLog log;
    const RemoteInputAccessPolicy policy = MakePolicy(RemoteInputMode::Disabled, "", log);

    EXPECT_FALSE(policy.IsAllowed(Address("127.0.0.1")));
    EXPECT_FALSE(policy.IsAllowed(Address("::1")));
}

TEST(RemoteInputAccessPolicy, LocalHostOnlyAllowsLoopbackOnly)
{
    TestLog log;
    const RemoteInputAccessPolicy policy = MakePolicy(RemoteInputMode::LocalHostOnly, "", log);

    EXPECT_TRUE(policy.IsAllowed(Address("127.0.0.1")));
    EXPECT_TRUE(policy.IsAllowed(Address("127.1.2.3")));
    EXPECT_TRUE(policy.IsAllowed(Address("::1")));
    EXPECT_TRUE(policy.IsAllowed(Address("::ffff:127.0.0.1")));
    EXPECT_FALSE(policy.IsAllowed(Address("192.168.1.42")));
    EXPECT_FALSE(policy.IsAllowed(Address("fe80::1")));
}

TEST(RemoteInputAccessPolicy, LocalHostOnlyIgnoresAllowedClients)
{
    TestLog log;
    const RemoteInputAccessPolicy policy = MakePolicy(RemoteInputMode::LocalHostOnly, "192.168.1.42", log);

    EXPECT_FALSE(policy.IsAllowed(Address("192.168.1.42")));
}

TEST(RemoteInputAccessPolicy, LanAllowsListedClientsAndLoopback)
{
    TestLog log;
    const RemoteInputAccessPolicy policy = MakePolicy(RemoteInputMode::Lan, "192.168.1.42, 10.0.0.0/8", log);

    EXPECT_TRUE(policy.IsAllowed(Address("192.168.1.42")));
    EXPECT_TRUE(policy.IsAllowed(Address("::ffff:192.168.1.42")));
    EXPECT_TRUE(policy.IsAllowed(Address("10.9.8.7")));
    EXPECT_TRUE(policy.IsAllowed(Address("127.0.0.1")));
    EXPECT_TRUE(policy.IsAllowed(Address("::1")));
    EXPECT_FALSE(policy.IsAllowed(Address("192.168.1.43")));
    EXPECT_FALSE(policy.IsAllowed(Address("8.8.8.8")));

    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Info, "Allowing clients from 192.168.1.42")) << log.Dump();
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Info, "Allowing clients from 10.0.0.0/8")) << log.Dump();
    EXPECT_EQ(log.Count(RemoteInputLogLevel::Warning), 0u) << log.Dump();
}

TEST(RemoteInputAccessPolicy, LanWithoutAllowedClientsAllowsLoopbackOnly)
{
    TestLog log;
    const RemoteInputAccessPolicy policy = MakePolicy(RemoteInputMode::Lan, "", log);

    EXPECT_TRUE(policy.IsAllowed(Address("127.0.0.1")));
    EXPECT_FALSE(policy.IsAllowed(Address("192.168.1.42")));
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Warning, "no usable entries")) << log.Dump();
}

TEST(RemoteInputAccessPolicy, LanNeverAllowsInternetAddresses)
{
    TestLog log;
    const RemoteInputAccessPolicy policy = MakePolicy(RemoteInputMode::Lan,
        "8.8.8.8, 0.0.0.0/0, ::/0, 2001:db8::/32, 10.0.0.0/7, 192.168.0.0/15", log);

    EXPECT_FALSE(policy.IsAllowed(Address("8.8.8.8")));
    EXPECT_FALSE(policy.IsAllowed(Address("2001:db8::1")));
    EXPECT_FALSE(policy.IsAllowed(Address("10.0.0.1")));      // only listed as part of the too-wide 10.0.0.0/7
    EXPECT_FALSE(policy.IsAllowed(Address("192.168.1.1")));   // only listed as part of the too-wide 192.168.0.0/15
    EXPECT_EQ(log.Count(RemoteInputLogLevel::Warning), 7u) << log.Dump();   // six entries, then "no usable entries"
}

TEST(RemoteInputAccessPolicy, LanSkipsBadEntriesButKeepsGoodOnes)
{
    TestLog log;
    const RemoteInputAccessPolicy policy = MakePolicy(RemoteInputMode::Lan,
        "my laptop, 192.168.1.42, 192.168.1, *, a..b", log);

    EXPECT_TRUE(policy.IsAllowed(Address("192.168.1.42")));
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Warning, "\"my laptop\"")) << log.Dump();
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Warning, "\"192.168.1\"")) << log.Dump();
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Warning, "not an IP address, CIDR range or host name")) << log.Dump();
    EXPECT_EQ(log.Count(RemoteInputLogLevel::Warning), 4u) << log.Dump();
}

TEST(RemoteInputAccessPolicy, LanAllowsHostNamesOnceTheirAddressesAreKnown)
{
    TestLog log;
    RemoteInputAccessPolicy policy = MakePolicy(RemoteInputMode::Lan, "my-phone, 10.0.0.1", log);

    EXPECT_EQ(policy.HostNames(), std::vector<std::string>{ "my-phone" });
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Info, "Allowing clients named \"my-phone\"")) << log.Dump();
    EXPECT_FALSE(policy.IsAllowed(Address("192.168.1.50")));

    policy.SetHostAddresses({ Address("192.168.1.50"), Address("fe80::1234") });
    EXPECT_TRUE(policy.IsAllowed(Address("192.168.1.50")));
    EXPECT_TRUE(policy.IsAllowed(Address("::ffff:192.168.1.50")));
    EXPECT_TRUE(policy.IsAllowed(Address("fe80::1234%2")));
    EXPECT_TRUE(policy.IsAllowed(Address("10.0.0.1")));

    // A new lookup replaces the old addresses.
    policy.SetHostAddresses({ Address("192.168.1.51") });
    EXPECT_FALSE(policy.IsAllowed(Address("192.168.1.50")));
    EXPECT_TRUE(policy.IsAllowed(Address("192.168.1.51")));
    EXPECT_TRUE(policy.IsAllowed(Address("10.0.0.1")));
}

TEST(RemoteInputAccessPolicy, LooksUpHostNamesOnlyForLocalNetworkClients)
{
    TestLog log;
    const RemoteInputAccessPolicy policy = MakePolicy(RemoteInputMode::Lan, "my-phone", log);

    // A local network client could be the phone at a new address; nobody else could be allowed by any name.
    EXPECT_TRUE(policy.CouldBeAllowedByName(Address("192.168.1.50")));
    EXPECT_TRUE(policy.CouldBeAllowedByName(Address("fe80::1234")));
    EXPECT_FALSE(policy.CouldBeAllowedByName(Address("8.8.8.8")));
    EXPECT_FALSE(policy.CouldBeAllowedByName(Address("2001:db8::1")));

    // Without host names, or outside LAN mode, there is nothing to look up.
    EXPECT_FALSE(MakePolicy(RemoteInputMode::Lan, "192.168.1.42", log).CouldBeAllowedByName(Address("192.168.1.50")));
    EXPECT_FALSE(MakePolicy(RemoteInputMode::LocalHostOnly, "my-phone", log)
        .CouldBeAllowedByName(Address("192.168.1.50")));
}

TEST(RemoteInputAccessPolicy, HostNamesNeverAllowInternetAddresses)
{
    TestLog log;
    RemoteInputAccessPolicy policy = MakePolicy(RemoteInputMode::Lan, "my-phone", log);

    policy.SetHostAddresses({ Address("8.8.8.8"), Address("2001:db8::1"), Address("192.168.1.50") });
    EXPECT_FALSE(policy.IsAllowed(Address("8.8.8.8")));
    EXPECT_FALSE(policy.IsAllowed(Address("2001:db8::1")));
    EXPECT_TRUE(policy.IsAllowed(Address("192.168.1.50")));
}

TEST(RemoteInputAccessPolicy, LanSaysLocalhostIsNotNeeded)
{
    TestLog log;
    const RemoteInputAccessPolicy policy = MakePolicy(RemoteInputMode::Lan, "LocalHost", log);

    EXPECT_TRUE(policy.HostNames().empty());
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Info, "is not needed")) << log.Dump();
}

TEST(RemoteInputAccessPolicy, HostNamesAreIgnoredOutsideLanMode)
{
    TestLog log;
    RemoteInputAccessPolicy policy = MakePolicy(RemoteInputMode::LocalHostOnly, "my-phone", log);

    EXPECT_TRUE(policy.HostNames().empty());
    policy.SetHostAddresses({ Address("192.168.1.50") });
    EXPECT_FALSE(policy.IsAllowed(Address("192.168.1.50")));
}

TEST(IsValidHostName, AcceptsHostNames)
{
    for (const std::string_view name : { "my-phone", "LAPTOP-ABC123", "marks-iphone.local", "pc.example.com",
        "pc.example.com.", "my_pc", "a", "x1", "1x", "a.b.c" })
    {
        EXPECT_TRUE(IsValidHostName(name)) << name;
    }
    EXPECT_TRUE(IsValidHostName(std::string(63, 'a') + ".local"));
}

TEST(IsValidHostName, RejectsEverythingElse)
{
    for (const std::string_view name : { "", ".", "192.168.1", "10", "1.2.3.4.5", "my phone", "my-phone!", "-phone",
        "phone-", "a..b", ".phone", "phone..", "pc/24", "fe80::1", "*", "a.-b", "a-.b", "caf\xC3\xA9" })
    {
        EXPECT_FALSE(IsValidHostName(name)) << '"' << name << '"';
    }
    EXPECT_FALSE(IsValidHostName(std::string(64, 'a') + ".local"));
    EXPECT_FALSE(IsValidHostName(std::string(254, 'a')));
}

TEST(RemoteInputAccessPolicy, LanToleratesSpacesAndEmptyEntries)
{
    TestLog log;
    const RemoteInputAccessPolicy policy = MakePolicy(RemoteInputMode::Lan, " 192.168.1.42 ,, \t10.0.0.1 ,", log);

    EXPECT_TRUE(policy.IsAllowed(Address("192.168.1.42")));
    EXPECT_TRUE(policy.IsAllowed(Address("10.0.0.1")));
    EXPECT_EQ(log.Count(RemoteInputLogLevel::Warning), 0u) << log.Dump();
}

TEST(RemoteInputAccessPolicy, LanSaysLoopbackEntriesAreNotNeeded)
{
    TestLog log;
    const RemoteInputAccessPolicy policy = MakePolicy(RemoteInputMode::Lan, "127.0.0.1, ::1, 192.168.1.42", log);

    EXPECT_TRUE(policy.IsAllowed(Address("127.0.0.1")));
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Info, "is not needed")) << log.Dump();
    EXPECT_EQ(log.Count(RemoteInputLogLevel::Warning), 0u) << log.Dump();
}

TEST(RemoteInputAccessPolicy, LanRejectsEntriesWithAPortOrBrackets)
{
    TestLog log;
    const RemoteInputAccessPolicy policy = MakePolicy(RemoteInputMode::Lan, "192.168.1.42:4242, [fe80::1], 10.0.0.1",
        log);

    EXPECT_FALSE(policy.IsAllowed(Address("192.168.1.42")));
    EXPECT_FALSE(policy.IsAllowed(Address("fe80::1")));
    EXPECT_TRUE(policy.IsAllowed(Address("10.0.0.1")));
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Warning, "\"192.168.1.42:4242\"")) << log.Dump();
    EXPECT_TRUE(log.Contains(RemoteInputLogLevel::Warning, "\"[fe80::1]\"")) << log.Dump();
    EXPECT_EQ(log.Count(RemoteInputLogLevel::Warning), 2u) << log.Dump();
}

TEST(RemoteInputAccessPolicy, AllowsLanClientsOnlyInLanModeWithUsableEntries)
{
    TestLog log;
    EXPECT_TRUE(MakePolicy(RemoteInputMode::Lan, "192.168.1.42", log).AllowsLanClients());
    EXPECT_TRUE(MakePolicy(RemoteInputMode::Lan, "my-phone", log).AllowsLanClients());

    EXPECT_FALSE(MakePolicy(RemoteInputMode::Lan, "", log).AllowsLanClients());
    EXPECT_FALSE(MakePolicy(RemoteInputMode::Lan, "8.8.8.8, localhost, 127.0.0.1, my phone", log).AllowsLanClients());
    EXPECT_FALSE(MakePolicy(RemoteInputMode::LocalHostOnly, "192.168.1.42", log).AllowsLanClients());
    EXPECT_FALSE(MakePolicy(RemoteInputMode::Disabled, "192.168.1.42", log).AllowsLanClients());
}

TEST(RemoteInputAccessPolicy, LanAcceptsIpv6LocalNetworkRanges)
{
    TestLog log;
    const RemoteInputAccessPolicy policy = MakePolicy(RemoteInputMode::Lan, "fe80::/10, fd00::/8", log);

    EXPECT_TRUE(policy.IsAllowed(Address("fe80::1234")));
    EXPECT_TRUE(policy.IsAllowed(Address("fd12::1")));
    EXPECT_FALSE(policy.IsAllowed(Address("2001:db8::1")));
}
