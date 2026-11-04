#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "RemoteInputLog.h"

// Records log messages so that tests can check what was logged, including messages written on the
// network thread.
class TestLog final : public RemoteInputLog
{
public:
    void Write(RemoteInputLogLevel level, std::string_view message) override;

    // True if a message at `level` containing `fragment` has been logged.
    bool Contains(RemoteInputLogLevel level, std::string_view fragment) const;

    // Waits until a message at `level` containing `fragment` has been logged, or `count` of them. Returns false on
    // timeout.
    bool WaitFor(RemoteInputLogLevel level, std::string_view fragment,
        std::chrono::milliseconds timeout = std::chrono::seconds(5));
    bool WaitFor(RemoteInputLogLevel level, std::string_view fragment, std::size_t count,
        std::chrono::milliseconds timeout = std::chrono::seconds(5));

    // Number of messages logged at `level`, or only those that contain `fragment`.
    std::size_t Count(RemoteInputLogLevel level) const;
    std::size_t Count(RemoteInputLogLevel level, std::string_view fragment) const;

    // Every message logged so far, one per line, for failure messages.
    std::string Dump() const;

private:
    struct Entry
    {
        RemoteInputLogLevel level;
        std::string message;
    };

    std::size_t CountLocked(RemoteInputLogLevel level, std::string_view fragment) const;

    mutable std::mutex mutex_;
    std::condition_variable written_;
    std::vector<Entry> entries_;   // guarded by mutex_
};
