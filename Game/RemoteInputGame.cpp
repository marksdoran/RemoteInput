// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: http://www.viva64.com
#include "RemoteInputGame.h"

#include <cstddef>
#include <format>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "FALCLIB/INCLUDE/ehandler.h"

#include "RemoteInputLog.h"
#include "RemoteInputServer.h"
#include "RemoteInputSettings.h"

// The game's config variables for remote input: the mode (0 = disabled, the default; 1 = local host only;
// 2 = LAN), the port, and the clients allowed in LAN mode.
extern int g_nCallbackSocketMode;
extern int g_nCallbackSocketPortNum;
extern char g_sCallbackSocketRemoteClients[];

namespace
{
// How many different unknown callback names are reported, each once, before the rest go unreported. A client
// sending made-up names cannot then fill the log.
constexpr std::size_t max_reported_unknown_callbacks = 64;

// Verbose messages, one per command received, are passed on in debug builds only. They help while trying remote
// input out, but the game already logs each callback it invokes, and busy clients could put hundreds of them a
// second into the log.
#if defined(_DEBUG)
constexpr bool log_verbose_messages = true;
#else
constexpr bool log_verbose_messages = false;
#endif

// Sends remote input's log to the game's debug log. MonoPrint() is already called from other threads (the Tacview
// server's, for one), so calling it from the network thread is nothing new.
class RemoteInputMonoPrintLog final : public RemoteInputLog
{
public:
    void Write(RemoteInputLogLevel level, std::string_view message) override
    {
        if constexpr (!log_verbose_messages)
        {
            if (level == RemoteInputLogLevel::Verbose)
            {
                return;
            }
        }
        const std::string_view level_name = RemoteInputLogLevelName(level);
        MonoPrint("RemoteInput [%.*s] %.*s\n", static_cast<int>(level_name.size()), level_name.data(),
            static_cast<int>(message.size()), message.data());
    }
};

// Both exist for the whole run of the program, so GetRemoteInput() never has to check for a server, and the input
// loop cannot race StartRemoteInput() or StopRemoteInput() over a pointer. Constructing the server opens no socket
// and starts no thread; StartRemoteInput() does that.
RemoteInputMonoPrintLog remote_input_log;
RemoteInputServer remote_input_server(remote_input_log);

// Destroyed just before the server. Stopping the server during static destruction would log through MonoPrint(),
// which may already be gone by then, so an exit path that skips StopRemoteInput() is caught here instead.
struct RemoteInputStopCheck
{
    RemoteInputStopCheck() = default;
    RemoteInputStopCheck(const RemoteInputStopCheck&) = delete;
    RemoteInputStopCheck(RemoteInputStopCheck&&) = delete;
    RemoteInputStopCheck& operator=(const RemoteInputStopCheck&) = delete;
    RemoteInputStopCheck& operator=(RemoteInputStopCheck&&) = delete;
    ~RemoteInputStopCheck()
    {
        INT3_ASSERT(!remote_input_server.IsRunning());
    }
} remote_input_stop_check;

// Only the input loop's thread uses this. Keeping it between frames avoids allocating every frame.
std::vector<RemoteInputCommand> received_commands;

// The unknown callback names reported so far. The input loop's thread reports them; StartRemoteInput() forgets
// them, on the thread that starts remote input, so that each run of remote input reports names afresh. The lock
// is only taken when a name is unknown, never on an ordinary frame.
std::mutex unknown_callbacks_mutex;
std::set<std::string, std::less<>> reported_unknown_callbacks;   // guarded by unknown_callbacks_mutex
bool has_stopped_reporting_unknown_callbacks = false;            // guarded by unknown_callbacks_mutex

// Logs a callback name that does not exist, once per name, so that a misbehaving client cannot fill the log.
void ReportUnknownCallback(const RemoteInputCommand& command)
{
    std::string message;
    {
        const std::lock_guard lock(unknown_callbacks_mutex);
        if (has_stopped_reporting_unknown_callbacks || reported_unknown_callbacks.contains(command.callback))
        {
            return;
        }
        if (reported_unknown_callbacks.size() == max_reported_unknown_callbacks)
        {
            has_stopped_reporting_unknown_callbacks = true;
            message = std::format(
                "Clients have asked for {} different callbacks that do not exist; no more will be reported",
                max_reported_unknown_callbacks);
        }
        else
        {
            reported_unknown_callbacks.insert(command.callback);
            // Only name characters reach this point, so the name is safe to log as it is.
            message = std::format(
                "Client {} asked for {}, but there is no callback of that name (reported once per name)",
                command.client_id, command.callback);
        }
    }
    remote_input_log.Write(RemoteInputLogLevel::Warning, message);
}

void ForgetReportedUnknownCallbacks()
{
    const std::lock_guard lock(unknown_callbacks_mutex);
    reported_unknown_callbacks.clear();
    has_stopped_reporting_unknown_callbacks = false;
}
} // namespace

bool StartRemoteInput()
{
    ForgetReportedUnknownCallbacks();
    const RemoteInputSettings settings = MakeRemoteInputSettings(g_nCallbackSocketMode, g_nCallbackSocketPortNum,
        g_sCallbackSocketRemoteClients, remote_input_log);
    return remote_input_server.Start(settings);
}

void StopRemoteInput()
{
    remote_input_server.Stop();
}

void GetRemoteInput(std::vector<RemoteInputCall>& calls)
{
    calls.clear();
    remote_input_server.DrainRemoteInput(received_commands);
    for (RemoteInputCommand& command : received_commands)
    {
        const InputFunctionType function = FindFunctionFromString(command.callback.c_str());
        if (function == nullptr)
        {
            ReportUnknownCallback(command);
            continue;
        }
        calls.push_back({ function, std::move(command) });
    }
}
