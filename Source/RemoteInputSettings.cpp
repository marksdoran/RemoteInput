// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: http://www.viva64.com
#include "RemoteInputSettings.h"

#include <format>

#include "RemoteInputLog.h"

std::string_view RemoteInputModeName(RemoteInputMode mode)
{
    switch (mode)
    {
    case RemoteInputMode::Disabled:
        return "disabled";
    case RemoteInputMode::LocalHostOnly:
        return "local host only";
    case RemoteInputMode::Lan:
        return "LAN";
    }
    return "unknown";
}

RemoteInputSettings MakeRemoteInputSettings(int mode, int port, std::string_view allowed_clients,
    RemoteInputLog& log)
{
    constexpr int lowest_port = 1;
    constexpr int highest_port = 65535;

    // Stays disabled unless every value checks out.
    RemoteInputSettings settings;

    const bool is_known_mode = mode >= static_cast<int>(RemoteInputMode::Disabled)
        && mode <= static_cast<int>(RemoteInputMode::Lan);
    if (!is_known_mode)
    {
        log.Write(RemoteInputLogLevel::Error, std::format(
            "Mode {} is not valid (0 = disabled, 1 = local host only, 2 = LAN); remote input is disabled", mode));
        return settings;
    }

    const auto requested_mode = static_cast<RemoteInputMode>(mode);
    if (requested_mode == RemoteInputMode::Disabled)
    {
        return settings;
    }

    if (port < lowest_port || port > highest_port)
    {
        log.Write(RemoteInputLogLevel::Error, std::format(
            "Port {} is not valid ({} to {}); remote input is disabled", port, lowest_port, highest_port));
        return settings;
    }

    settings.mode = requested_mode;
    settings.port = static_cast<std::uint16_t>(port);
    if (requested_mode == RemoteInputMode::Lan)
    {
        settings.allowed_clients = allowed_clients;
    }
    else if (!allowed_clients.empty())
    {
        log.Write(RemoteInputLogLevel::Info, "The allowed client list applies in LAN mode only; ignoring it");
    }
    return settings;
}
