#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "RemoteInputQueue.h"
#include "RemoteInputSettings.h"

class RemoteInputLog;

// Lets companion apps on this computer or on the local network (voice recognition, button boxes, tablet
// dashboards) trigger game callbacks by name. A client connects over TCP and sends one command per line, e.g.
// "SimEject\n" or "SimEject_Press\n"; the game collects whatever has arrived once per frame with
// DrainRemoteInput(). If a client disconnects while holding inputs down, the server queues their releases.
//
// Threading: a dedicated network thread does all of the socket work between Start() and Stop().
//   - Start(), Stop(), IsRunning() and Port() must all be called from one thread, normally the main thread.
//   - DrainRemoteInput() may be called from one other thread, normally the sim loop, at any time, including
//     before Start() and after Stop().
//
// This header deliberately does not include ASIO, so the game code that uses the server does not get the socket
// headers (and <windows.h>) along with it.
class RemoteInputServer final
{
public:
    // `log` must outlive the server.
    explicit RemoteInputServer(RemoteInputLog& log, const RemoteInputLimits& limits = {});
    ~RemoteInputServer();   // calls Stop()

    RemoteInputServer(const RemoteInputServer&) = delete;
    RemoteInputServer& operator=(const RemoteInputServer&) = delete;

    // Starts listening as the settings allow. Returns true if the server is now accepting connections, or false
    // if the settings disable remote input or no socket could be opened; the log says which.
    bool Start(const RemoteInputSettings& settings);

    // Disconnects every client and stops the network thread, returning once that thread has exited. Does nothing if
    // the server is not running. Commands that arrived before the call, and the releases of anything the clients
    // were still holding, can still be drained afterwards. Host names are only looked up when a local network
    // client the server does not recognise connects; if Stop() is called while such a lookup is under way, it waits
    // for the OS to finish it, which is normally quick but takes as long as name resolution does to time out.
    void Stop();

    bool IsRunning() const;

    // The port the server is listening on, or 0 when it is not running. Useful when the settings asked for port 0
    // (any free port), as the tests do.
    std::uint16_t Port() const;

    // Replaces the contents of `commands` with everything received since the previous call, oldest first. Never
    // waits for network activity. Pass the same vector every frame to avoid allocating.
    void DrainRemoteInput(std::vector<RemoteInputCommand>& commands);

private:
    struct Network;   // the sockets and the network thread; exists only while the server is running

    RemoteInputLog& log_;
    const RemoteInputLimits limits_;
    RemoteInputQueue queue_;
    RemoteInputClientId next_client_id_ = 1;   // kept across restarts, so that client ids are never reused
    std::unique_ptr<Network> network_;
};
