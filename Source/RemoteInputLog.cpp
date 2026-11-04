// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: http://www.viva64.com
#include "RemoteInputLog.h"

#include <format>

std::string_view RemoteInputLogLevelName(RemoteInputLogLevel level)
{
    switch (level)
    {
    case RemoteInputLogLevel::Verbose:
        return "verbose";
    case RemoteInputLogLevel::Info:
        return "info";
    case RemoteInputLogLevel::Warning:
        return "warning";
    case RemoteInputLogLevel::Error:
        return "error";
    }
    return "unknown";
}

std::string QuoteForRemoteInputLog(std::string_view text)
{
    // Enough to recognise what a client sent, without letting one message swamp the log.
    constexpr std::size_t max_shown_bytes = 64;

    const std::string_view shown = text.substr(0, max_shown_bytes);

    std::string quoted;
    quoted.reserve(shown.size() + 2);
    quoted += '"';
    for (const char character : shown)
    {
        const auto byte = static_cast<unsigned char>(character);
        // Quotes and backslashes are escaped too, so the quoted text is never ambiguous.
        const bool is_plain = byte >= 0x20 && byte < 0x7F && character != '"' && character != '\\';
        if (is_plain)
        {
            quoted += character;
        }
        else
        {
            quoted += std::format("\\x{:02X}", byte);
        }
    }
    quoted += '"';

    if (shown.size() < text.size())
    {
        quoted += std::format(" (first {} of {} bytes)", shown.size(), text.size());
    }
    return quoted;
}
