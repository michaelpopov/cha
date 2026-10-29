#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>

namespace cha {

// One live conversation's complete rendered catalogue; empty is a valid snapshot.
class SummaryPromptCache final {
public:
    using Reader = std::function<std::optional<std::string>(const std::atomic_bool&)>;
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    explicit SummaryPromptCache(Reader reader, Clock clock = std::chrono::steady_clock::now);
    std::string load(const std::atomic_bool& cancellation);

private:
    struct Entry {
        std::string section;
        std::chrono::steady_clock::time_point created_at;
    };
    Reader reader_;
    Clock clock_;
    std::mutex mutex_;
    std::optional<Entry> entry_;
};

} // namespace cha
