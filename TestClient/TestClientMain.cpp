// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: http://www.viva64.com

// Console test client for remote input. It stands in for a companion app, such as a voice recognition
// program: it connects to the game (or to RemoteInputTestServer) and sends every line typed as a command.
//
//   RemoteInputTestClient [host] [port]        defaults: 127.0.0.1 4242
//
// The host can be an IPv4 address, an IPv6 address such as ::1 (without brackets), or a name such as
// localhost. Lines that start with '/' are commands for this client; they can never be callback names.

#include <charconv>
#include <cstddef>
#include <format>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "ExtLibs/asio/include/asio.hpp"

namespace
{
void PrintLine(std::string_view text)
{
    std::cout << text << '\n' << std::flush;
}

void PrintHelp()
{
    PrintLine("Type a callback name, e.g. SimEject_Press, and press Enter to send it. Client commands:");
    PrintLine("  /crlf            switch between LF and CR LF line endings");
    PrintLine("  /repeat N name   send a command N times in one burst (the server takes 50 at once from a");
    PrintLine("                   connection, then slows the rest down to 50 a second)");
    PrintLine("  /partial text    send text without a line ending (the server should hold on to it)");
    PrintLine("  /help            show this help");
    PrintLine("  /quit            disconnect and exit");
}

// The server never sends anything, so if a read completes without waiting, the connection is over: either
// the server closed it (e.g. this client is not allowed, or the game is shutting down) or it was lost.
bool HasServerClosed(asio::ip::tcp::socket& socket)
{
    asio::error_code error;
    socket.non_blocking(true, error);
    char byte = 0;
    socket.read_some(asio::buffer(&byte, 1), error);
    asio::error_code ignored;
    socket.non_blocking(false, ignored);
    return error != asio::error::would_block;
}

bool Send(asio::ip::tcp::socket& socket, std::string_view data)
{
    if (HasServerClosed(socket))
    {
        PrintLine("The server has closed the connection; its log says why.");
        return false;
    }
    asio::error_code error;
    asio::write(socket, asio::buffer(data), error);
    if (error)
    {
        PrintLine(std::format("Sending failed: {}", error.message()));
        return false;
    }
    return true;
}

// Splits text at its first space, e.g. "/repeat 5 SimEject" into "/repeat" and "5 SimEject".
std::pair<std::string_view, std::string_view> SplitFirstWord(std::string_view text)
{
    const std::size_t space = text.find(' ');
    if (space == std::string_view::npos)
    {
        return { text, std::string_view() };
    }
    return { text.substr(0, space), text.substr(space + 1) };
}

// Handles one client command. Returns false when the client should exit.
bool HandleClientCommand(asio::ip::tcp::socket& socket, std::string_view line, std::string& line_ending)
{
    const auto [command, rest] = SplitFirstWord(line);

    if (command == "/quit" || command == "/exit")
    {
        return false;
    }
    if (command == "/crlf")
    {
        line_ending = line_ending == "\n" ? "\r\n" : "\n";
        PrintLine(line_ending == "\n" ? "Lines now end with LF" : "Lines now end with CR LF");
        return true;
    }
    if (command == "/repeat")
    {
        constexpr std::size_t max_count = 100000;
        const auto [count_text, name] = SplitFirstWord(rest);
        std::size_t count = 0;
        const char* const count_end = count_text.data() + count_text.size();
        const auto [parse_end, parse_error] = std::from_chars(count_text.data(), count_end, count);
        if (parse_error != std::errc() || parse_end != count_end || count > max_count || name.empty())
        {
            PrintLine(std::format("Usage: /repeat N name, with N up to {}", max_count));
            return true;
        }
        std::string burst;
        for (std::size_t index = 0; index < count; ++index)
        {
            burst += name;
            burst += line_ending;
        }
        return Send(socket, burst);
    }
    if (command == "/partial")
    {
        return Send(socket, rest);
    }
    if (command != "/help")
    {
        PrintLine(std::format("Unknown client command {}", command));
    }
    PrintHelp();
    return true;
}
} // namespace

int main(int argc, char* argv[])
{
    const std::span<char*> arguments(argv, static_cast<std::size_t>(argc));
    const std::string host = arguments.size() > 1 ? arguments[1] : "127.0.0.1";
    const std::string port = arguments.size() > 2 ? arguments[2] : "4242";

    asio::io_context io_context;
    asio::ip::tcp::resolver resolver(io_context);
    asio::error_code error;
    const asio::ip::tcp::resolver::results_type endpoints = resolver.resolve(host, port, error);
    if (error)
    {
        PrintLine(std::format("Cannot find {} port {}: {}", host, port, error.message()));
        return 1;
    }

    // Tries each address the host name resolved to, e.g. ::1 and then 127.0.0.1 for localhost.
    asio::ip::tcp::socket socket(io_context);
    const asio::ip::tcp::endpoint server = asio::connect(socket, endpoints, error);
    if (error)
    {
        PrintLine(std::format("Cannot connect to {} port {}: {}", host, port, error.message()));
        return 1;
    }
    PrintLine(std::format("Connected to {} port {}.", server.address().to_string(), server.port()));
    PrintHelp();

    std::string line_ending = "\n";
    std::string line;
    while (std::getline(std::cin, line))
    {
        const bool keep_going = line.starts_with('/')
            ? HandleClientCommand(socket, line, line_ending)
            : Send(socket, line + line_ending);
        if (!keep_going)
        {
            break;
        }
    }

    asio::error_code ignored;
    socket.shutdown(asio::ip::tcp::socket::shutdown_both, ignored);
    socket.close(ignored);
    return 0;
}
