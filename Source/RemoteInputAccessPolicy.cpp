// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: http://www.viva64.com
#include "RemoteInputAccessPolicy.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <format>
#include <system_error>
#include <utility>

#include "RemoteInputLog.h"

namespace
{
constexpr unsigned int ipv4_address_bits = 32;
constexpr unsigned int ipv6_address_bits = 128;
// An IPv4-mapped IPv6 address is ::ffff: followed by the IPv4 address, which takes the last 32 bits.
constexpr unsigned int ipv4_mapped_prefix_bits = ipv6_address_bits - ipv4_address_bits;

unsigned int AddressBits(const asio::ip::address& address)
{
    return address.is_v4() ? ipv4_address_bits : ipv6_address_bits;
}

// True for a decimal number from 0 to 255 written without leading zeros.
bool IsDecimalByte(std::string_view text)
{
    constexpr unsigned int max_byte = 255;
    unsigned int value = 0;
    const char* const end = text.data() + text.size();
    const auto [parse_end, error] = std::from_chars(text.data(), end, value);
    const bool has_leading_zero = text.size() > 1 && text.front() == '0';
    return error == std::errc() && parse_end == end && !has_leading_zero && value <= max_byte;
}

// True for an IPv4 address written as four decimal bytes separated by dots, e.g. 192.168.1.42: the one form that
// every platform reads the same way. Windows also reads shorthand forms that the others reject: "192.168.1" as
// 192.168.0.1, a lone number as a whole address, and a leading 0 as octal.
bool IsDottedDecimal(std::string_view text)
{
    constexpr std::size_t byte_count = 4;
    for (std::size_t index = 0; index < byte_count; ++index)
    {
        const std::size_t dot = text.find('.');
        const bool is_last_byte = index + 1 == byte_count;
        if (is_last_byte != (dot == std::string_view::npos) || !IsDecimalByte(text.substr(0, dot)))
        {
            return false;
        }
        text.remove_prefix(is_last_byte ? text.size() : dot + 1);
    }
    return true;
}

// True for an IP address written in a standard form, perhaps followed by an IPv6 scope id (as in fe80::1%3).
// Windows' parser also accepts a port ("192.168.1.42:4242"), brackets ("[fe80::1]") and the shorthand IPv4 forms
// above, which the others reject; turning them all away keeps the allowed client list meaning the same everywhere.
bool IsStandardAddressText(std::string_view text)
{
    const std::string_view address = text.substr(0, text.find('%'));
    const std::size_t last_colon = address.rfind(':');
    if (last_colon == std::string_view::npos)
    {
        return address.size() == text.size() && IsDottedDecimal(address);   // IPv4, which has no scope id
    }

    // IPv6: hexadecimal groups and colons, which may end with an IPv4 address, as in ::ffff:192.168.1.5. A dot
    // anywhere before the last colon means something else, such as an IPv4 address with a port.
    const std::size_t first_dot = address.find('.');
    if (address.find_first_not_of("0123456789ABCDEFabcdef:.") != std::string_view::npos)
    {
        return false;
    }
    return first_dot == std::string_view::npos
        || (first_dot > last_colon && IsDottedDecimal(address.substr(last_colon + 1)));
}

// Zeroes every bit after the first `prefix_length` bits of an address in network byte order.
template <typename Bytes>
Bytes ClearBitsAfterPrefix(Bytes bytes, unsigned int prefix_length)
{
    for (std::size_t index = 0; index < bytes.size(); ++index)
    {
        const std::size_t first_bit = index * 8;
        if (prefix_length <= first_bit)
        {
            bytes[index] = 0;
        }
        else if (prefix_length < first_bit + 8)
        {
            const std::size_t kept_bits = prefix_length - first_bit;
            bytes[index] = static_cast<unsigned char>(bytes[index] & (0xFFu << (8 - kept_bits)));
        }
    }
    return bytes;
}

asio::ip::address ClearHostBits(const asio::ip::address& address, unsigned int prefix_length)
{
    if (address.is_v4())
    {
        return asio::ip::address_v4(ClearBitsAfterPrefix(address.to_v4().to_bytes(), prefix_length));
    }
    // Rebuilding the address also drops any IPv6 scope id (the "%3" in fe80::1%3), which is not part of the
    // address range.
    return asio::ip::address_v6(ClearBitsAfterPrefix(address.to_v6().to_bytes(), prefix_length));
}

// The address blocks reserved for local networks. Remote input never accepts clients outside them.
const std::vector<RemoteInputAddressRange>& LanRanges()
{
    using Ipv4Bytes = asio::ip::address_v4::bytes_type;
    using Ipv6Bytes = asio::ip::address_v6::bytes_type;

    static const std::vector<RemoteInputAddressRange> ranges
    {
        // RFC 1918 private networks, as used by home routers.
        RemoteInputAddressRange(asio::ip::address_v4(Ipv4Bytes{ 10, 0, 0, 0 }), 8),
        RemoteInputAddressRange(asio::ip::address_v4(Ipv4Bytes{ 172, 16, 0, 0 }), 12),
        RemoteInputAddressRange(asio::ip::address_v4(Ipv4Bytes{ 192, 168, 0, 0 }), 16),
        // IPv4 link-local, used when no DHCP server answers.
        RemoteInputAddressRange(asio::ip::address_v4(Ipv4Bytes{ 169, 254, 0, 0 }), 16),
        // IPv6 unique local (fc00::/7) and link-local (fe80::/10). The bytes not listed are zero.
        RemoteInputAddressRange(asio::ip::address_v6(Ipv6Bytes{ 0xFC }), 7),
        RemoteInputAddressRange(asio::ip::address_v6(Ipv6Bytes{ 0xFE, 0x80 }), 10),
    };
    return ranges;
}

bool IsWithinLan(const RemoteInputAddressRange& range)
{
    return std::ranges::any_of(LanRanges(), [&range](const RemoteInputAddressRange& lan_range)
        {
            return lan_range.Contains(range);
        });
}

bool IsLoopbackRange(const RemoteInputAddressRange& range)
{
    static const RemoteInputAddressRange ipv4_loopback(asio::ip::address_v4::loopback(), 8);
    static const RemoteInputAddressRange ipv6_loopback(asio::ip::address_v6::loopback(), ipv6_address_bits);
    return ipv4_loopback.Contains(range) || ipv6_loopback.Contains(range);
}

std::string_view TrimSpaces(std::string_view text)
{
    constexpr std::string_view spaces = " \t";
    const std::size_t first = text.find_first_not_of(spaces);
    if (first == std::string_view::npos)
    {
        return {};
    }
    const std::size_t last = text.find_last_not_of(spaces);
    return text.substr(first, last - first + 1);
}

// Splits a comma-separated list, trimming spaces and tabs around each entry and skipping empty entries.
std::vector<std::string_view> SplitList(std::string_view list)
{
    std::vector<std::string_view> entries;
    while (!list.empty())
    {
        const std::size_t comma = list.find(',');
        const std::string_view entry = TrimSpaces(list.substr(0, comma));
        if (!entry.empty())
        {
            entries.push_back(entry);
        }
        if (comma == std::string_view::npos)
        {
            break;
        }
        list.remove_prefix(comma + 1);
    }
    return entries;
}

bool EqualsIgnoringCase(std::string_view text, std::string_view lowercase)
{
    return std::ranges::equal(text, lowercase, [](char text_character, char lowercase_character)
        {
            const char lowered = text_character >= 'A' && text_character <= 'Z'
                ? static_cast<char>(text_character - 'A' + 'a')
                : text_character;
            return lowered == lowercase_character;
        });
}

// Adds an address or CIDR entry of the allowed client list to `ranges`, or logs why it is unusable.
void AddAllowedRange(std::string_view entry, const RemoteInputAddressRange& range,
    std::vector<RemoteInputAddressRange>& ranges, RemoteInputLog& log)
{
    if (IsLoopbackRange(range))
    {
        log.Write(RemoteInputLogLevel::Info, std::format(
            "Allowed client {} is not needed: programs on this computer can always connect",
            QuoteForRemoteInputLog(entry)));
        return;
    }
    if (!IsWithinLan(range))
    {
        log.Write(RemoteInputLogLevel::Warning, std::format(
            "Ignoring allowed client {}: only local network addresses can be allowed (10.0.0.0/8, 172.16.0.0/12, "
            "192.168.0.0/16, 169.254.0.0/16, fc00::/7 and fe80::/10)",
            QuoteForRemoteInputLog(entry)));
        return;
    }

    log.Write(RemoteInputLogLevel::Info, std::format("Allowing clients from {}", range.ToString()));
    ranges.push_back(range);
}

// Adds a host name entry of the allowed client list to `host_names`, unless it is not needed.
void AddAllowedHostName(std::string_view entry, std::vector<std::string>& host_names, RemoteInputLog& log)
{
    if (EqualsIgnoringCase(entry, "localhost"))
    {
        log.Write(RemoteInputLogLevel::Info, std::format(
            "Allowed client {} is not needed: programs on this computer can always connect",
            QuoteForRemoteInputLog(entry)));
        return;
    }

    log.Write(RemoteInputLogLevel::Info, std::format(
        "Allowing clients named {}; the name is looked up when a local network client that is not otherwise "
        "allowed connects", QuoteForRemoteInputLog(entry)));
    host_names.emplace_back(entry);
}
} // namespace

std::optional<RemoteInputAddressRange> RemoteInputAddressRange::Parse(std::string_view text)
{
    const std::size_t slash = text.find('/');
    const std::string_view address_text = text.substr(0, slash);
    if (!IsStandardAddressText(address_text))
    {
        return std::nullopt;
    }

    // Any scope id is left out: matching ignores it, and platforms differ in which forms of it they accept.
    asio::error_code error;
    asio::ip::address address = asio::ip::make_address(address_text.substr(0, address_text.find('%')), error);
    if (error)
    {
        return std::nullopt;
    }

    unsigned int prefix_length = AddressBits(address);
    if (slash != std::string_view::npos)
    {
        const std::string_view prefix_text = text.substr(slash + 1);
        const char* const prefix_end = prefix_text.data() + prefix_text.size();
        const auto [parse_end, parse_error] = std::from_chars(prefix_text.data(), prefix_end, prefix_length);
        if (parse_error != std::errc() || parse_end != prefix_end || prefix_length > AddressBits(address))
        {
            return std::nullopt;
        }
    }

    // Store an IPv4-mapped IPv6 range as the IPv4 range it covers, to match NormalizeAddress().
    if (address.is_v6() && address.to_v6().is_v4_mapped())
    {
        if (prefix_length < ipv4_mapped_prefix_bits)
        {
            return std::nullopt;   // would also cover IPv6 addresses that are not IPv4-mapped
        }
        address = NormalizeAddress(address);
        prefix_length -= ipv4_mapped_prefix_bits;
    }

    return RemoteInputAddressRange(address, prefix_length);
}

RemoteInputAddressRange::RemoteInputAddressRange(const asio::ip::address& address, unsigned int prefix_length) :
    network_(ClearHostBits(address, prefix_length)),
    prefix_length_(prefix_length < AddressBits(address) ? prefix_length : AddressBits(address))
{
}

bool RemoteInputAddressRange::Contains(const asio::ip::address& address) const
{
    const asio::ip::address candidate = NormalizeAddress(address);
    if (candidate.is_v4() != network_.is_v4())
    {
        return false;
    }
    return ClearHostBits(candidate, prefix_length_) == network_;
}

bool RemoteInputAddressRange::Contains(const RemoteInputAddressRange& range) const
{
    return range.prefix_length_ >= prefix_length_ && Contains(range.network_);
}

std::string RemoteInputAddressRange::ToString() const
{
    if (prefix_length_ == AddressBits(network_))
    {
        return network_.to_string();
    }
    return std::format("{}/{}", network_.to_string(), prefix_length_);
}

bool IsLanAddress(const asio::ip::address& address)
{
    return std::ranges::any_of(LanRanges(), [&address](const RemoteInputAddressRange& lan_range)
        {
            return lan_range.Contains(address);
        });
}

asio::ip::address NormalizeAddress(const asio::ip::address& address)
{
    if (address.is_v6() && address.to_v6().is_v4_mapped())
    {
        const asio::ip::address_v6::bytes_type bytes = address.to_v6().to_bytes();
        return asio::ip::address_v4(asio::ip::address_v4::bytes_type{ bytes[12], bytes[13], bytes[14], bytes[15] });
    }
    return address;
}

bool IsValidHostName(std::string_view text)
{
    constexpr std::size_t max_name_length = 253;
    constexpr std::size_t max_label_length = 63;

    if (!text.empty() && text.back() == '.')
    {
        text.remove_suffix(1);   // a fully qualified name may end with the dot of the DNS root
    }
    if (text.empty() || text.size() > max_name_length)
    {
        return false;
    }

    bool has_letter = false;
    std::size_t label_length = 0;
    char previous = '.';
    for (const char character : text)
    {
        if (character == '.')
        {
            if (label_length == 0 || previous == '-')
            {
                return false;   // an empty label, or one that ends with '-'
            }
            label_length = 0;
        }
        else
        {
            const bool is_letter = (character >= 'A' && character <= 'Z') || (character >= 'a' && character <= 'z');
            const bool is_digit = character >= '0' && character <= '9';
            if (!is_letter && !is_digit && character != '-' && character != '_')
            {
                return false;
            }
            if (character == '-' && label_length == 0)
            {
                return false;   // a label that starts with '-'
            }
            ++label_length;
            if (label_length > max_label_length)
            {
                return false;
            }
            has_letter = has_letter || is_letter;
        }
        previous = character;
    }
    return has_letter && label_length > 0 && previous != '-';
}

RemoteInputAccessPolicy RemoteInputAccessPolicy::Create(const RemoteInputSettings& settings, RemoteInputLog& log)
{
    std::vector<RemoteInputAddressRange> allowed_ranges;
    std::vector<std::string> host_names;
    if (settings.mode == RemoteInputMode::Lan)
    {
        for (const std::string_view entry : SplitList(settings.allowed_clients))
        {
            if (const std::optional<RemoteInputAddressRange> range = RemoteInputAddressRange::Parse(entry))
            {
                AddAllowedRange(entry, *range, allowed_ranges, log);
            }
            else if (IsValidHostName(entry))
            {
                AddAllowedHostName(entry, host_names, log);
            }
            else
            {
                log.Write(RemoteInputLogLevel::Warning, std::format(
                    "Ignoring allowed client {}: it is not an IP address, CIDR range or host name",
                    QuoteForRemoteInputLog(entry)));
            }
        }
        if (allowed_ranges.empty() && host_names.empty())
        {
            log.Write(RemoteInputLogLevel::Warning,
                "LAN mode is on, but the allowed client list has no usable entries, so only programs on this "
                "computer can connect");
        }
    }
    return RemoteInputAccessPolicy(settings.mode, std::move(allowed_ranges), std::move(host_names));
}

RemoteInputAccessPolicy::RemoteInputAccessPolicy(RemoteInputMode mode,
    std::vector<RemoteInputAddressRange> allowed_ranges, std::vector<std::string> host_names) :
    mode_(mode),
    allowed_ranges_(std::move(allowed_ranges)),
    host_names_(std::move(host_names))
{
}

bool RemoteInputAccessPolicy::IsAllowed(const asio::ip::address& client_address) const
{
    if (mode_ == RemoteInputMode::Disabled)
    {
        return false;
    }

    const asio::ip::address client = NormalizeAddress(client_address);
    if (client.is_loopback())
    {
        return true;
    }
    if (mode_ != RemoteInputMode::Lan)
    {
        return false;
    }
    const auto contains_client = [&client](const RemoteInputAddressRange& range)
        {
            return range.Contains(client);
        };
    return std::ranges::any_of(allowed_ranges_, contains_client)
        || std::ranges::any_of(host_addresses_, contains_client);
}

bool RemoteInputAccessPolicy::CouldBeAllowedByName(const asio::ip::address& client_address) const
{
    return mode_ == RemoteInputMode::Lan && !host_names_.empty() && IsLanAddress(client_address);
}

bool RemoteInputAccessPolicy::AllowsLanClients() const
{
    return mode_ == RemoteInputMode::Lan && (!allowed_ranges_.empty() || !host_names_.empty());
}

void RemoteInputAccessPolicy::SetHostAddresses(const std::vector<asio::ip::address>& addresses)
{
    host_addresses_.clear();
    for (const asio::ip::address& address : addresses)
    {
        // Whatever a name resolves to, only local network addresses can ever be allowed.
        const asio::ip::address normalized = NormalizeAddress(address);
        if (IsLanAddress(normalized))
        {
            host_addresses_.emplace_back(normalized, normalized.is_v4() ? ipv4_address_bits : ipv6_address_bits);
        }
    }
}
