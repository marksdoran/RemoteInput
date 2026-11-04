// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: http://www.viva64.com
#include "RemoteInputLineProtocol.h"

#include <algorithm>

namespace
{
constexpr std::string_view press_suffix = "_Press";
constexpr std::string_view release_suffix = "_Release";

// True if `command` is a non-empty name followed by `suffix`.
bool HasSuffix(std::string_view command, std::string_view suffix)
{
    return command.size() > suffix.size() && command.ends_with(suffix);
}
} // namespace

RemoteInputLine ParseRemoteInputLine(std::string_view line)
{
    if (!line.empty() && line.back() == '\r')
    {
        line.remove_suffix(1);
    }

    if (line.empty())
    {
        return { RemoteInputLineKind::Empty, line };
    }
    if (!std::ranges::all_of(line, IsRemoteInputNameCharacter))
    {
        return { RemoteInputLineKind::Invalid, line };
    }
    return { RemoteInputLineKind::Command, line };
}

std::string_view RemoveByteOrderMark(std::string_view first_line)
{
    constexpr std::string_view byte_order_mark = "\xEF\xBB\xBF";
    if (first_line.starts_with(byte_order_mark))
    {
        first_line.remove_prefix(byte_order_mark.size());
    }
    return first_line;
}

bool IsRemoteInputNameCharacter(char character)
{
    return (character >= 'A' && character <= 'Z')
        || (character >= 'a' && character <= 'z')
        || (character >= '0' && character <= '9')
        || character == '_';
}

RemoteInputCallbackRequest SplitRemoteInputCommand(std::string_view command)
{
    if (HasSuffix(command, press_suffix))
    {
        command.remove_suffix(press_suffix.size());
        return { command, RemoteInputAction::Press };
    }
    if (HasSuffix(command, release_suffix))
    {
        command.remove_suffix(release_suffix.size());
        return { command, RemoteInputAction::Release };
    }
    return { command, RemoteInputAction::PressAndRelease };
}

std::string MakeRemoteInputCommand(std::string_view callback, RemoteInputAction action)
{
    std::string command(callback);
    switch (action)
    {
    case RemoteInputAction::PressAndRelease:
        break;
    case RemoteInputAction::Press:
        command += press_suffix;
        break;
    case RemoteInputAction::Release:
        command += release_suffix;
        break;
    }
    return command;
}

std::string_view RemoteInputActionName(RemoteInputAction action)
{
    switch (action)
    {
    case RemoteInputAction::PressAndRelease:
        return "press and release";
    case RemoteInputAction::Press:
        return "press";
    case RemoteInputAction::Release:
        return "release";
    }
    return "unknown";
}
