# RemoteInput prototype

RemoteInput lets companion apps (voice recognition, button boxes, tablet dashboards) trigger game callbacks by
name over TCP, instead of faking key presses or going through vJoy. A client connects and sends one command per
line, e.g. `SimEject` or `SimEject_Press`; the game picks up whatever has arrived once per frame.

This folder holds the component itself (`Source`), the game-side glue (`Game`), two console apps to try it with,
and GoogleTest tests.

## Before the first build

ASIO is not included here. Copy the game's `ExtLibs\asio` folder (v1.28.0) to `RemoteInput\ExtLibs\asio`, so
that `RemoteInput\ExtLibs\asio\include\asio.hpp` exists. The sources include it as
`"ExtLibs/asio/include/asio.hpp"`, as the game does. If it is missing, the build stops and says so.

## Build and test

1. Open `RemoteInput.sln` in Visual Studio 2022, choose x64 (or ARM64, if the ARM64 build tools are
   installed) and build the solution.
2. Run the tests from Test Explorer, or run `Bin\x64\Debug\RemoteInputTests.exe`.
   Some tests run the server in LAN mode, which listens on every interface, so Windows Firewall may ask about
   `RemoteInputTests.exe` the first time. Either answer is fine; the tests only connect from this computer.
   A few tests skip themselves on a computer without IPv6, a local network address or a network connection.
   Visual Studio's own Google Test adapter shows a skipped test as failed, with "This test has probably
   CRASHED" followed by the reason it skipped; that is a long-standing bug in the adapter, not a crash.
3. The projects build at warning level 4 without "treat warnings as errors". Once the MSVC build is known to be
   free of warnings, switch on `TreatWarningAsError` in `RemoteInput.props`.

## Try it

| Command | What it does |
|---|---|
| `RemoteInputTestServer` | Listens on 127.0.0.1 and ::1, port 4242, and shows the callbacks the "game" invokes. Console commands: `list`, `clear`, `help`, `quit`. |
| `RemoteInputTestClient` | Connects to 127.0.0.1:4242 and sends each line you type. `/help` lists its own commands (CR LF endings, bursts, unfinished lines). |
| `RemoteInputTestClient ::1` | The same over IPv6. A name such as `localhost` also works. |
| `RemoteInputTestServer --mode 2 --allow "192.168.1.0/24, my-phone"` | LAN mode: also accepts clients from that range, and the device named `my-phone`. |
| `RemoteInputTestClient 192.168.1.10` | From another computer: connects to the game PC at that address (`ipconfig` shows it). |

Both apps are in `Bin\<platform>\<configuration>`. The server's options (`--mode 0|1|2`, `--port N`,
`--allow "list"`) stand in for the game's config variables; it defaults to local host only, unlike the game. The
stand-in game knows six callbacks (`AFBrakesToggle`, `AFGearToggle`, `SimEject`, `SimPickle`,
`SimTriggerFirstDetent`, `SimTriggerSecondDetent`); any other name is logged as unknown, once, as the game will
do. The client's `/repeat` sends a burst; past the first 50 commands, the server slows it down.

To test over the LAN, start the server in mode 2 with the other computer's address, name or LAN range in
`--allow`. The first time, Windows Firewall asks whether `RemoteInputTestServer.exe` may accept connections;
allow it on private networks. Copy `RemoteInputTestClient.exe` to the other computer and connect. A client that
is not allowed is refused, and the server's log says so.

## Protocol, for companion app authors

- TCP, port 4242 by default. One command per line, ending in LF or CR LF. The server never sends anything back.
- A command is a callback name: letters, digits and `_` only, at most 256 bytes counting a CR.
  - `SimEject`: a complete key stroke.
  - `SimEject_Press` and `SimEject_Release`: the key goes down and, later, comes back up.
- If a client disconnects while holding something down, the game releases it. Each connection can hold up to
  64 inputs down at once.
- Empty lines are ignored, as is a UTF-8 byte order mark at the very start of a connection. A line with other
  characters is logged and ignored. A line that is too long gets the client disconnected. An unfinished line when
  a client disconnects is discarded, so a cut-off `SimEject_Release` can never act as `SimEject`.
- Keep one connection open while the app runs, rather than connecting for each command.
- Each connection can send 50 commands at once, and 50 a second after that. A connection that sends faster is
  slowed down: the server reads from it only as fast as that, and TCP makes the app's sends wait. Nothing is
  dropped for being too fast, and releases of held inputs are never held back. When connections are being made
  more than 4 times a second (after a first 16), a new one gets no head start: its allowance builds up from
  nothing, so its commands wait about 20 ms each at first.
- Up to 16 connections at once. Up to 1024 commands can wait for the game; any more are dropped and logged,
  except releases of held inputs, which always get through.

## Layout

| Path | Contents |
|---|---|
| `Source` | The component. The game uses it through `RemoteInputServer.h`, `RemoteInputSettings.h` and `RemoteInputLog.h`; none of them include ASIO. |
| `Game` | The game-side glue: `StartRemoteInput()`, `StopRemoteInput()` and `GetRemoteInput()`. It moves into the game with the component. |
| `EngineStubs` | Stand-ins for the game code that the component and the glue call (`INT3_ASSERT`, `MonoPrint`, `FindFunctionFromString`), for the standalone build only. |
| `TestServer`, `TestClient` | The two console apps. |
| `Tests` | GoogleTest unit tests, plus end-to-end tests over real sockets. |
| `ExtLibs` | GoogleTest 1.17.0 and its project file; ASIO goes here too (see above). |
| `CMakeLists.txt` | GCC/Clang build, used to check portability and to run the sanitizers. Not used on Windows. |

## Design

```
   local voice app                   phone or laptop on the LAN
   127.0.0.1 or ::1                  address or name in the allowed list
      |                                 |
      v                                 v
 +----------------------------------------------------------------+
 | network thread: one std::thread running asio::io_context       |
 |                                                                |
 |  RemoteInputListener   access policy, host names, limits       |
 |    |                                                           |
 |    v                                                           |
 |  RemoteInputSession    one per connection: frames, checks      |
 |                        lines, tracks held inputs, rate limit   |
 +----+-----------------------------------------------------------+
      |
      v
   RemoteInputQueue   mutex-guarded and bounded: the only shared state
      |
      v
   game thread        GetRemoteInput() once per frame, from the input loop
```

- **Threads.** One network thread does all the socket work with ASIO completion handlers, so the listener and
  sessions need no locks. The queue is the only object that the network thread and the game share; its lock is
  held only to append one command or to swap two vectors, so `GetRemoteInput()` never waits for the network. The
  log is written from both sides, so whatever the game logs through must be thread-safe; the glue uses
  `MonoPrint()`, which the Tacview server already calls from its own thread. Host name lookups run on a helper
  thread that ASIO manages; their results come back to the network thread.
- **Lifetime.** `Start()` queues the first accepts before starting the thread (otherwise `io_context::run()`
  could find no work and return at once). `Stop()` closes the sockets on the network thread, waits for it to
  finish, and returns; the destructor calls `Stop()`. Client ids carry on across a stop and start, so a command
  still waiting in the game can never be confused with one from a new client.
- **Held inputs.** Each session remembers what its client has pressed and not released. When the connection
  ends, for whatever reason, the session queues the releases, so a dropped connection never leaves an input
  held down. Releases of held inputs are never held back by the rate limit, nor dropped when the queue is full.
- **Security.** Mode 0 opens no socket. Mode 1 listens on the loopback addresses only, so other computers cannot
  even connect. Mode 2 listens on every interface and accepts this computer plus the allowed list; if the list
  has no usable entry, it listens on loopback only, as mode 1 does. Only local network addresses can be allowed
  (10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16, 169.254.0.0/16, fc00::/7, fe80::/10), so remote input never
  reaches the internet, even through a port forward or a host name that resolves to an internet address. Bad
  config values disable remote input or are skipped; they never widen access. There is no encryption or
  authentication, and a host name is only as trustworthy as name resolution on the network; an address is
  stricter.
- **Host names.** Names in the allowed list are looked up only when they are needed: when a client connects from
  a local network address that is not otherwise allowed, which is how a phone with a new DHCP address shows up.
  That client waits for the answer and is then let in or refused. Lookups are at least 5 seconds apart; a client
  that arrives sooner waits for the next one. A client from outside the local network ranges could never be
  allowed, so it is refused without a lookup. Changes in what a name resolves to are logged. The OS cannot
  interrupt a lookup, so a `Stop()` that comes while one is running waits for it to finish: normally a moment,
  but as long as name resolution takes to time out if the network's name server is not answering.
- **Limits.** Line length, connections, commands per second per connection, queue length and held inputs per
  connection are all capped (`RemoteInputLimits`), so a misbehaving client cannot use up memory or give the game
  much work each frame. A connection over its rate is paused rather than having commands dropped: its session
  stops reading until the allowance has built up, with a timer that `Stop()` cancels. A client that reconnects in
  a loop is still let in, but gains nothing by it and locks nobody else out. One program can still open all 16
  connections; these limits guard against mistakes, not against a hostile program on the player's own computer.
- **Logging.** `RemoteInputLog` receives every message with a level: verbose (each command), info (start, stop,
  connect, disconnect, host name results), warning (refused clients, bad lines, dropped commands, unknown
  callbacks, bad config entries) and error (cannot listen, invalid settings). Text from clients is quoted and
  escaped before it is logged. Clients decide how often the messages about them happen, so those are throttled:
  twenty at once, then about one a second, with the number left out logged when the server stops. Unknown
  callback names are logged once per name, for up to 64 names, each time remote input starts. The glue passes
  verbose messages to `MonoPrint()` in debug builds only.
- **No exceptions.** Nothing catches exceptions, and nothing relies on them. ASIO is only called through the
  overloads that report errors in an `asio::error_code`, wherever it has one. What can still throw only does so
  when the process runs out of resources: memory (`std::bad_alloc`), a thread (`std::thread`'s
  `std::system_error`), or the OS objects that ASIO cannot work without, such as its event queue. With nothing to
  catch them, these end in `std::terminate()`, which the crash handler reports. A game built with
  `ASIO_NO_EXCEPTIONS` must define `asio::detail::throw_exception()`; routing it to `INT3_ASSERT` keeps the
  crashlog path (`Tests/AsioNoExceptions.h` shows the shape). The CMake build checks that everything builds,
  and passes its tests, with exceptions disabled.
- **Port reuse.** On Linux and the like, the server sets `SO_REUSEADDR`, so it can listen again straight after a
  restart. On Windows it sets nothing: Windows already allows that, and `SO_EXCLUSIVEADDRUSE`, which would stop
  another program running as the player from taking the port over, can keep the port unusable for minutes after
  a restart. A program running as the player has easier ways to interfere with the game anyway.
- **Extending.** `RemoteInputLineProtocol` is the only code that knows the wire format. Callback names cannot
  contain spaces, so a future form such as `Axis Pitch 0.25` can be added without affecting existing clients.

## Integrating into the game

1. Add the files in `Source` and `Game` to the game's project, with the module's precompiled header first in
   each `.cpp` as usual. `EngineStubs` stays behind. In `Game/RemoteInputGame.h`, replace the `GameStubs.h`
   include with the game headers that declare `InputFunctionType` and `FindFunctionFromString()`, and include the
   one that declares `MonoPrint()` in `Game/RemoteInputGame.cpp`.
2. Define the config variables, with the mode defaulting to disabled:

   ```cpp
   int g_nCallbackSocketMode = 0;              // 0 = disabled, 1 = local host only, 2 = LAN
   int g_nCallbackSocketPortNum = 4242;
   char g_sCallbackSocketRemoteClients[256] = "";   // addresses, CIDR ranges and host names, comma-separated
   ```

3. Call `StartRemoteInput()` on the WinMain startup path and `StopRemoteInput()` on the shutdown path. The glue
   asserts, during static destruction, that `StopRemoteInput()` was called.
4. In the input loop, next to the keyboard, mouse, joystick and XInput handling:

   ```cpp
   GetRemoteInput(remote_input_calls);   // a std::vector<RemoteInputCall> kept from frame to frame
   for (const RemoteInputCall& call : remote_input_calls)
   {
       // Invoke call.function the way a key binding would: call.command.action says whether this is a full
       // key stroke, a press or a release.
   }
   ```

   Two things for the game side to decide: which callbacks may be invoked remotely (an allow list, so that
   nobody on the LAN can trigger, say, quit), and what to do with presses that arrive while no mission is
   running (ignore them, perhaps using `call.command.received_at`). Releases must always be invoked, even when
   presses are being ignored: a release may be the only thing that lets go of an input the game is holding.

## Checked so far

On Linux with GCC 13 and Clang 18, at `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
-Werror`: everything builds cleanly, and all the tests pass, run repeatedly in random order, under
AddressSanitizer with UBSan, under ThreadSanitizer, and with exceptions disabled. clang-tidy's bugprone,
performance, concurrency, misc and core guidelines checks were run over everything; what they still report is
false positives (ASIO error codes that are deliberately ignored, and asynchronous reads taken for recursion) and
nits in the test apps. The apps were also run against each other, in local host and LAN modes, including
clients held for a host name lookup, refused LAN and non-LAN clients, and stopping while a client waits. The LAN
tests ran with a local network address added for the purpose.

On Windows x64 with Visual Studio 2022, the solution builds and the tests run from Test Explorer, including
IPv6 and the LAN tests. The first run there found that Windows read shorthand addresses such as "192.168.1" in
the allowed client list, which the other platforms reject; only standard forms are accepted now. Not checked
yet: ARM64, Wine, and PVS-Studio.
