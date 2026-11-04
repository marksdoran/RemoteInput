#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

class RemoteInputLog;

// Who may connect. The values are the ones the player writes in the config file.
enum class RemoteInputMode
{
    Disabled = 0,       // no socket is opened; this is the default
    LocalHostOnly = 1,  // listen on the loopback interface only, so only apps on this computer can connect
    Lan = 2,            // also accept clients on the local network that are in the allowed client list
};

// Readable name of a mode for log messages, e.g. "local host only".
std::string_view RemoteInputModeName(RemoteInputMode mode);

// The player-facing settings, converted from the game's config variables by MakeRemoteInputSettings().
struct RemoteInputSettings
{
    RemoteInputMode mode = RemoteInputMode::Disabled;
    std::uint16_t port = 0;         // 0 asks the OS for any free port; only the tests do that
    std::string allowed_clients;    // comma-separated IP addresses, CIDR ranges and host names; LAN mode only
};

// Fixed resource limits. These are not player settings: they bound how much memory and attention a
// misbehaving client can take. The tests lower them to reach the limits quickly.
struct RemoteInputLimits
{
    std::size_t max_line_length = 256;        // bytes per line, counting a CR but not the LF
    std::size_t max_connections = 16;         // clients connected at once; further connections are refused
    std::size_t max_queued_commands = 1024;   // commands waiting for the game; further commands are dropped
    std::size_t max_held_inputs = 64;         // inputs one connection can hold down at once; more presses are dropped
    // Commands each connection can send per second, on average and in a single burst. Far more than a person can
    // produce, but it caps the work one connection can give the game each frame, so that it cannot crowd out the
    // others. A connection that sends faster is slowed down: the server reads from it only as fast as this allows.
    // A program with several connections gets this much for each of them, up to max_connections in all.
    double max_commands_per_second = 50.0;
    // New connections per second that may send a burst of commands straight away, after a first max_connections
    // of them. Connections beyond that are still accepted, but start with no allowance and build it up at
    // max_commands_per_second, so a client that reconnects in a loop gains nothing by it, and locks nobody out.
    double max_new_connections_per_second = 4.0;
    // Shortest time between two lookups of the allowed host names, so that a refused client that keeps
    // reconnecting cannot turn into a stream of name lookups.
    std::chrono::steady_clock::duration host_lookup_interval = std::chrono::seconds(5);
};

// Converts the raw config values into settings. Any invalid value disables remote input and logs why: a
// typo in the config file must never open the port more widely than the player intended.
RemoteInputSettings MakeRemoteInputSettings(int mode, int port, std::string_view allowed_clients,
    RemoteInputLog& log);
