// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: http://www.viva64.com

// Console test harness for remote input. It plays the part of the game: it owns the config variables, starts and
// stops remote input through the game-side glue (Game/RemoteInputGame.h), and collects remote input once per frame
// on a thread of its own, the way the input loop will.
//
//   RemoteInputTestServer [--mode 0|1|2] [--port N] [--allow "192.168.1.42, 10.0.0.0/24, my-phone"]
//
// Console commands: list, clear, help, quit.

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <format>
#include <functional>
#include <iostream>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "GameStubs.h"
#include "RemoteInputGame.h"
#include "RemoteInputLineProtocol.h"

// Stand-ins for the game's config variables, which the game defines and fills from its config file. In the game
// the mode must default to 0 (disabled); here it defaults to local host only, so the app is useful without
// arguments.
int g_nCallbackSocketMode = 1;
int g_nCallbackSocketPortNum = 4242;
char g_sCallbackSocketRemoteClients[256] = "";

namespace
{
// Prints one line through the game's debug log (here, the console), so that it cannot interleave with the
// remote input log written from the network thread.
void PrintLine(const std::string& text)
{
    MonoPrint("%s\n", text.c_str());
}

// What the "game" has invoked, for the list and clear console commands. The frame thread adds to it while the
// main thread reads and clears it.
class InvokedCallbacks final
{
public:
    void Add(std::string description)
    {
        const std::lock_guard lock(mutex_);
        descriptions_.push_back(std::move(description));
    }

    std::vector<std::string> Copy() const
    {
        const std::lock_guard lock(mutex_);
        return descriptions_;
    }

    void Clear()
    {
        const std::lock_guard lock(mutex_);
        descriptions_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> descriptions_;
};

// Stands in for the input loop: once per frame, takes the callbacks remote clients asked for and "invokes" them.
void RunFrameLoop(std::stop_token stop_token, InvokedCallbacks& invoked)
{
    constexpr std::chrono::milliseconds frame_time{ 16 };

    std::vector<RemoteInputCall> calls;
    while (!stop_token.stop_requested())
    {
        GetRemoteInput(calls);
        const auto now = std::chrono::steady_clock::now();
        for (const RemoteInputCall& call : calls)
        {
            const std::string description = std::format("{} ({})", call.command.callback,
                RemoteInputActionName(call.command.action));
            const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(now - call.command.received_at);
            PrintLine(std::format("Game: invoke {} for client {}, {} ms after it arrived", description,
                call.command.client_id, waited.count()));
            invoked.Add(description);
        }
        std::this_thread::sleep_for(frame_time);
    }
}

void PrintUsage()
{
    PrintLine("Usage: RemoteInputTestServer [--mode 0|1|2] [--port N] "
        "[--allow \"192.168.1.42, 10.0.0.0/24, my-phone\"]");
    PrintLine("  --mode   0 = disabled, 1 = local host only (default here), 2 = LAN");
    PrintLine("  --port   port to listen on (default 4242)");
    PrintLine("  --allow  clients allowed in mode 2: IP addresses, CIDR ranges and host names, comma-separated");
}

void PrintHelp()
{
    PrintLine("Commands: list (what the game has invoked), clear (empty that list), help, quit");
    PrintLine("Callbacks this stand-in knows: AFBrakesToggle, AFGearToggle, SimEject, SimPickle, "
        "SimTriggerFirstDetent, SimTriggerSecondDetent");
}

bool ParseInt(std::string_view text, int& value)
{
    const char* const end = text.data() + text.size();
    const auto [parse_end, error] = std::from_chars(text.data(), end, value);
    return error == std::errc() && parse_end == end;
}

// Copies `text` into a config string variable, the way the game's config loader fills char arrays.
template <std::size_t Size>
bool CopyConfigString(std::string_view text, char (&variable)[Size])
{
    if (text.size() >= Size)
    {
        return false;
    }
    std::memcpy(variable, text.data(), text.size());
    variable[text.size()] = '\0';
    return true;
}

// Sets the config variables from the command line, standing in for the game's config file. Returns false, after
// explaining why, if the command line is not usable.
bool ApplyCommandLine(std::span<char*> arguments)
{
    for (std::size_t index = 1; index < arguments.size(); ++index)
    {
        const std::string_view option = arguments[index];
        if (option == "--help" || option == "-h" || option == "/?")
        {
            PrintUsage();
            return false;
        }
        if (index + 1 >= arguments.size())
        {
            PrintLine(std::format("{} needs a value", option));
            PrintUsage();
            return false;
        }
        const std::string_view value = arguments[++index];

        bool is_valid = false;
        if (option == "--mode")
        {
            is_valid = ParseInt(value, g_nCallbackSocketMode);
        }
        else if (option == "--port")
        {
            is_valid = ParseInt(value, g_nCallbackSocketPortNum);
        }
        else if (option == "--allow")
        {
            is_valid = CopyConfigString(value, g_sCallbackSocketRemoteClients);
        }
        else
        {
            PrintLine(std::format("Unknown option {}", option));
            PrintUsage();
            return false;
        }

        if (!is_valid)
        {
            PrintLine(std::format("Bad value for {}: {}", option, value));
            return false;
        }
    }
    return true;
}

std::string_view TrimSpaces(std::string_view text)
{
    constexpr std::string_view spaces = " \t\r";
    const std::size_t first = text.find_first_not_of(spaces);
    if (first == std::string_view::npos)
    {
        return {};
    }
    return text.substr(first, text.find_last_not_of(spaces) - first + 1);
}

void PrintInvoked(const InvokedCallbacks& invoked)
{
    const std::vector<std::string> descriptions = invoked.Copy();
    PrintLine(std::format("The game has invoked {} callback(s) since the list was last cleared", descriptions.size()));
    for (std::size_t index = 0; index < descriptions.size(); ++index)
    {
        PrintLine(std::format("  {:4}  {}", index + 1, descriptions[index]));
    }
}
} // namespace

int main(int argc, char* argv[])
{
    if (!ApplyCommandLine(std::span<char*>(argv, static_cast<std::size_t>(argc))))
    {
        return 2;
    }
    if (!StartRemoteInput())
    {
        PrintLine("Remote input is not running; the messages above say why. Use --help to see the options.");
        return 1;
    }

    InvokedCallbacks invoked;
    std::jthread frame_loop(RunFrameLoop, std::ref(invoked));
    PrintHelp();

    std::string line;
    while (std::getline(std::cin, line))
    {
        const std::string_view command = TrimSpaces(line);
        if (command == "quit" || command == "exit")
        {
            break;
        }
        if (command == "list")
        {
            PrintInvoked(invoked);
        }
        else if (command == "clear")
        {
            invoked.Clear();
            PrintLine("Cleared the list of invoked callbacks");
        }
        else if (command == "help" || command.empty())
        {
            PrintHelp();
        }
        else
        {
            PrintLine(std::format("Unknown command \"{}\"", command));
            PrintHelp();
        }
    }

    // Shut down in the game's order: the input loop stops first, then remote input.
    frame_loop.request_stop();
    frame_loop.join();
    StopRemoteInput();
    return 0;
}
