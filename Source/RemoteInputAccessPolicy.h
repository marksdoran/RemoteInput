#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "ExtLibs/asio/include/asio.hpp"

#include "RemoteInputSettings.h"

class RemoteInputLog;

// A block of IP addresses in CIDR notation, e.g. 192.168.1.0/24 or fd00::/8. A single address is a block whose
// prefix covers the whole address, e.g. 192.168.1.42/32.
class RemoteInputAddressRange final
{
public:
    // Parses "address" or "address/prefix-length", IPv4 or IPv6. Host bits past the prefix are ignored, so
    // 192.168.1.42/24 means 192.168.1.0/24, and so is an IPv6 scope id. Returns nothing if the text is not a valid
    // range. Only the standard forms are accepted (an IPv4 address is four decimal numbers), so that the text
    // means the same on every platform.
    static std::optional<RemoteInputAddressRange> Parse(std::string_view text);

    RemoteInputAddressRange(const asio::ip::address& address, unsigned int prefix_length);

    bool Contains(const asio::ip::address& address) const;
    bool Contains(const RemoteInputAddressRange& range) const;   // true if all of `range` is inside this one

    // Canonical form, e.g. "192.168.1.0/24", or just the address for a single address.
    std::string ToString() const;

private:
    asio::ip::address network_;     // first address of the block: every bit past the prefix is zero
    unsigned int prefix_length_;    // number of leading bits shared by every address in the block
};

// True for addresses that can only belong to a local network: private (RFC 1918) and link-local IPv4 addresses,
// and unique-local and link-local IPv6 addresses. Loopback addresses are not included.
bool IsLanAddress(const asio::ip::address& address);

// Converts an IPv4-mapped IPv6 address (::ffff:a.b.c.d) into the IPv4 address it carries, so that every client
// has exactly one form of address. Other addresses are returned unchanged.
asio::ip::address NormalizeAddress(const asio::ip::address& address);

// True for text that can be looked up as a host name: dot-separated labels of letters, digits, '-' and '_', with
// at least one letter, 253 characters at most. Requiring a letter keeps a mistyped address such as "192.168.1"
// away from the resolver, which would read it as 192.168.0.1 in an old shorthand. Hexadecimal forms such as
// "0xa.1" still pass, and the resolver may read those as addresses too; that is harmless, because whatever a name
// resolves to is still subject to the local-network-only rule.
bool IsValidHostName(std::string_view text);

// Decides which clients may use remote input:
//   - nobody when remote input is disabled;
//   - programs on this computer (loopback addresses) in both local host only and LAN modes;
//   - in LAN mode, also the clients in the allowed client list, given as addresses, CIDR ranges or host names.
//     Only local network addresses can be allowed, so remote input is never reachable from the internet, even
//     through a port forward or a host name that resolves to an internet address.
class RemoteInputAccessPolicy final
{
public:
    // Builds the policy for the given settings. Entries of the allowed client list that are malformed or not on
    // the local network are logged and skipped, so a bad entry can never make the policy more permissive.
    static RemoteInputAccessPolicy Create(const RemoteInputSettings& settings, RemoteInputLog& log);

    bool IsAllowed(const asio::ip::address& client_address) const;

    // True if a client that is not allowed might be once the host names have been looked up again, e.g. a phone
    // that has a new address: the mode is LAN, the allowed client list has host names, and the client's address is
    // a local network one. For anyone else, looking the names up would be pointless.
    bool CouldBeAllowedByName(const asio::ip::address& client_address) const;

    // True if anyone besides programs on this computer could be allowed: the mode is LAN and the allowed client list
    // has at least one usable entry.
    bool AllowsLanClients() const;

    // The host names in the allowed client list. They are looked up elsewhere; SetHostAddresses() then tells
    // the policy what they resolved to.
    const std::vector<std::string>& HostNames() const
    {
        return host_names_;
    }

    // Replaces the addresses the host names resolved to. Addresses that are not on the local network are ignored.
    void SetHostAddresses(const std::vector<asio::ip::address>& addresses);

private:
    RemoteInputAccessPolicy(RemoteInputMode mode, std::vector<RemoteInputAddressRange> allowed_ranges,
        std::vector<std::string> host_names);

    RemoteInputMode mode_;
    std::vector<RemoteInputAddressRange> allowed_ranges_;   // all inside the LAN address blocks
    std::vector<std::string> host_names_;
    std::vector<RemoteInputAddressRange> host_addresses_;   // what host_names_ last resolved to, one per address
};
