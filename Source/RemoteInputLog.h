#pragma once

#include <string>
#include <string_view>

// How important a remote input log message is. The game decides which levels it shows; the test apps
// show them all.
enum class RemoteInputLogLevel
{
    Verbose,    // routine traffic, e.g. every command received
    Info,       // lifecycle: started, stopped, client connected or disconnected
    Warning,    // a client, a line of input or a config entry was rejected; remote input keeps running
    Error,      // remote input cannot do what it was configured to do, e.g. the port is already in use
};

// Where remote input sends its log messages. The game forwards them to its own log; the test apps print
// them to the console and the unit tests record them.
//
// Messages are complete sentences without a trailing newline or a "remote input" prefix; the
// implementation adds whatever decoration its destination needs.
//
// Write() is called from the network thread as well as from the game's own threads (the one that starts
// and stops the server, and the one that collects remote input), so implementations must be thread-safe.
class RemoteInputLog
{
public:
    RemoteInputLog(const RemoteInputLog&) = delete;
    RemoteInputLog& operator=(const RemoteInputLog&) = delete;
    virtual ~RemoteInputLog() = default;

    virtual void Write(RemoteInputLogLevel level, std::string_view message) = 0;

protected:
    RemoteInputLog() = default;
};

// Short lowercase name of a level, e.g. "warning", for implementations that print the level.
std::string_view RemoteInputLogLevelName(RemoteInputLogLevel level);

// Makes untrusted text, such as a line received from a client, safe to put in a log message. The result is
// wrapped in double quotes, anything other than printable ASCII is shown as a \xNN escape, and long text is
// shortened, so a client cannot inject control characters into the log or make one message huge. (How many
// messages clients can cause is limited separately; see RemoteInputLogThrottle.)
std::string QuoteForRemoteInputLog(std::string_view text);
