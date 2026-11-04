// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: http://www.viva64.com
#include "GameStubs.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>

namespace
{
void StubCallback(unsigned long /*value*/, int /*state*/, void* /*button*/)
{
}

// A few of the game's callback names: enough to exercise the lookup.
constexpr const char* known_callbacks[] =
{
    "AFBrakesToggle",
    "AFGearToggle",
    "SimEject",
    "SimPickle",
    "SimTriggerFirstDetent",
    "SimTriggerSecondDetent",
};

const std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();

std::mutex monoprint_mutex;
bool is_recording = false;                 // guarded by monoprint_mutex
std::vector<std::string> recorded_lines;   // guarded by monoprint_mutex
} // namespace

InputFunctionType FindFunctionFromString(const char* str)
{
    for (const char* name : known_callbacks)
    {
        if (std::strcmp(name, str) == 0)
        {
            return &StubCallback;
        }
    }
    return nullptr;
}

void MonoPrint(const char* format, ...)
{
    char text[1024];
    std::va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(text, sizeof(text), format, arguments);
    va_end(arguments);

    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start_time;
    const std::lock_guard lock(monoprint_mutex);
    std::printf("[%8.3f] %s", elapsed.count(), text);
    std::fflush(stdout);
    if (is_recording)
    {
        recorded_lines.emplace_back(text);
    }
}

void StartRecordingMonoPrint()
{
    const std::lock_guard lock(monoprint_mutex);
    is_recording = true;
    recorded_lines.clear();
}

std::vector<std::string> TakeRecordedMonoPrint()
{
    const std::lock_guard lock(monoprint_mutex);
    return std::exchange(recorded_lines, {});
}
