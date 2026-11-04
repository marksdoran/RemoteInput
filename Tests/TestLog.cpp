// This is an independent project of an individual developer. Dear PVS-Studio, please check it.
// PVS-Studio Static Code Analyzer for C, C++, C#, and Java: http://www.viva64.com
#include "TestLog.h"

#include <algorithm>
#include <format>

void TestLog::Write(RemoteInputLogLevel level, std::string_view message)
{
    {
        const std::lock_guard lock(mutex_);
        entries_.push_back({ level, std::string(message) });
    }
    written_.notify_all();
}

bool TestLog::Contains(RemoteInputLogLevel level, std::string_view fragment) const
{
    return Count(level, fragment) > 0;
}

bool TestLog::WaitFor(RemoteInputLogLevel level, std::string_view fragment, std::chrono::milliseconds timeout)
{
    return WaitFor(level, fragment, 1, timeout);
}

bool TestLog::WaitFor(RemoteInputLogLevel level, std::string_view fragment, std::size_t count,
    std::chrono::milliseconds timeout)
{
    std::unique_lock lock(mutex_);
    return written_.wait_for(lock, timeout, [&]
        {
            return CountLocked(level, fragment) >= count;
        });
}

std::size_t TestLog::Count(RemoteInputLogLevel level) const
{
    const std::lock_guard lock(mutex_);
    return static_cast<std::size_t>(std::ranges::count(entries_, level, &Entry::level));
}

std::size_t TestLog::Count(RemoteInputLogLevel level, std::string_view fragment) const
{
    const std::lock_guard lock(mutex_);
    return CountLocked(level, fragment);
}

std::string TestLog::Dump() const
{
    const std::lock_guard lock(mutex_);
    std::string dump = "Log:\n";
    for (const Entry& entry : entries_)
    {
        dump += std::format("  [{}] {}\n", RemoteInputLogLevelName(entry.level), entry.message);
    }
    return dump;
}

std::size_t TestLog::CountLocked(RemoteInputLogLevel level, std::string_view fragment) const
{
    return static_cast<std::size_t>(std::ranges::count_if(entries_, [&](const Entry& entry)
        {
            return entry.level == level && entry.message.find(fragment) != std::string::npos;
        }));
}
