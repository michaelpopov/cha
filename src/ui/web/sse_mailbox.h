#pragma once

#include "ui/web/web_session_runtime.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <variant>

namespace cha::web {

using SsePayload = std::variant<SnapshotEvent, AppendEvent>;

// The only cross-thread presentation queue for one runtime.  Its producer is
// the owner thread and its consumer is the HTTP streaming thread.
class SseMailbox final : public WebSnapshotSink {
public:
    using Stream = SseStreamToken;
    struct Next {
        bool open{};
        std::shared_ptr<const SsePayload> payload;
    };

    [[nodiscard]] Stream begin_stream(SnapshotEvent snapshot);
    [[nodiscard]] Next next(
        Stream stream,
        std::chrono::milliseconds heartbeat_interval);
    void written(Stream stream) noexcept;
    std::size_t end_stream(Stream stream) noexcept;

    void publish(SnapshotEvent snapshot) override;
    void publish_append(
        WebAppendCandidate candidate,
        const SessionSnapshot& fallback_snapshot) override;
    [[nodiscard]] bool wait_for_written(
        std::chrono::milliseconds deadline) override;
    void close() noexcept override;

private:
    void publish_snapshot_locked(SnapshotEvent snapshot);
    void publish_append_locked(
        WebAppendCandidate candidate,
        const SessionSnapshot& fallback_snapshot);

    std::mutex mutex_;
    std::condition_variable changed_;
    bool closed_{};
    std::uint64_t active_stream_{};
    std::uint64_t next_stream_{1};
    std::shared_ptr<const SsePayload> in_flight_;
    std::shared_ptr<const SsePayload> pending_;
    std::optional<AppendTarget> target_;
    std::uint64_t next_sequence_{};
    std::size_t collapsed_payloads_{};
};

} // namespace cha::web
