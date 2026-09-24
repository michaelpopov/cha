#pragma once

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cha {

struct AudioChunk {
    std::string mime_type;
    std::string body;
    bool complete{true};
    bool failed{};
};

// One growing clip, shared by its download and any players. Reads never wait
// for the provider, so the native UI thread can safely serve a small chunk.
class AudioStream {
public:
    void append(std::string_view mime_type, std::string_view bytes) {
        std::lock_guard lock(mutex_);
        if (done_ || failed_) return;
        if (bytes.size() > 256 * 1024 * 1024 - body_.size())
            throw std::runtime_error("Audio is too large.");
        mime_type_ = mime_type;
        body_.append(bytes);
    }
    void finish() { std::lock_guard lock(mutex_); done_ = true; }
    void fail() { std::lock_guard lock(mutex_); failed_ = true; }
    bool empty() const { std::lock_guard lock(mutex_); return body_.empty(); }

    std::optional<AudioChunk> read(std::uint64_t offset, std::size_t limit = 64 * 1024) const {
        std::lock_guard lock(mutex_);
        if (offset > body_.size()) return std::nullopt;
        const auto count = std::min(limit, body_.size() - static_cast<std::size_t>(offset));
        return AudioChunk{mime_type_, failed_ ? "" : body_.substr(offset, count),
            done_ && offset + count == body_.size(), failed_};
    }

private:
    mutable std::mutex mutex_;
    std::string mime_type_{"application/octet-stream"};
    std::string body_;
    bool done_{};
    bool failed_{};
};

} // namespace cha
