#pragma once

#include <string>
#include <string_view>

// The remote input wire format. A client sends one command per line, followed by LF or CR LF. A command is the
// name of a game callback, e.g. "SimEject", optionally with a suffix that asks for only half of a key stroke:
// "SimEject_Press" or "SimEject_Release". Names use only A-Z, a-z, 0-9 and '_'.
//
// This file is the only place that knows the format. Commands with parameters, axis values and the like can be
// added here later; because today's names cannot contain spaces, a future "Verb arguments" form cannot be
// confused with a callback name, and existing companion apps keep working.

// What a received line turned out to be.
enum class RemoteInputLineKind
{
    Command,    // a callback name, possibly with a _Press or _Release suffix
    Empty,      // nothing but a line ending; ignored without comment, so clients may send blank keep-alives
    Invalid,    // has characters that cannot appear in a callback name; logged and ignored
};

struct RemoteInputLine
{
    RemoteInputLineKind kind = RemoteInputLineKind::Empty;
    std::string_view text;   // the line without its line ending; points into the caller's buffer
};

// Interprets one line received from a client. `line` must not include the terminating LF; a CR before it, from
// a CR LF line ending, is removed here.
RemoteInputLine ParseRemoteInputLine(std::string_view line);

// Removes a UTF-8 byte order mark from the start of a connection's first line, if it has one. Some text writers,
// such as .NET's StreamWriter with Encoding.UTF8, put one at the start of everything they send.
std::string_view RemoveByteOrderMark(std::string_view first_line);

// True for the characters allowed in a callback name: A-Z, a-z, 0-9 and '_'. Unlike std::isalnum() this ignores
// the C locale and is safe for any char value.
bool IsRemoteInputNameCharacter(char character);

// What a command asks the game to do with its callback.
enum class RemoteInputAction
{
    PressAndRelease,   // "SimEject": a complete key stroke
    Press,             // "SimEject_Press": the key goes down and stays down
    Release,           // "SimEject_Release": the key comes back up
};

struct RemoteInputCallbackRequest
{
    std::string_view callback;   // the callback name without its suffix, e.g. "SimEject"
    RemoteInputAction action = RemoteInputAction::PressAndRelease;
};

// Splits a command into its callback name and action. A suffix on its own, such as "_Press", is taken as a
// callback name rather than as a suffix.
RemoteInputCallbackRequest SplitRemoteInputCommand(std::string_view command);

// The command a client would send for `callback` and `action`, e.g. "SimEject_Release".
std::string MakeRemoteInputCommand(std::string_view callback, RemoteInputAction action);

// Readable name of an action for log messages, e.g. "press".
std::string_view RemoteInputActionName(RemoteInputAction action);
