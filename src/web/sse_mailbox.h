#pragma once

#include "web/protocol.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <variant>

namespace cha::web {

using SsePayload = std::variant<SnapshotEvent, AppendEvent>;

// Whether the mailbox represented one controller-proven append exactly, or
// needs the owner to publish a current full snapshot instead.
enum class AppendPublishResult {
    Accepted,
    SnapshotRequired,
};

// The only cross-thread presentation queue for one live session.  Its producer
// is the owner thread and its consumer is the HTTP streaming thread.
class SseMailbox final {
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

    void publish(SnapshotEvent snapshot);
    // A pending snapshot cannot safely coexist with a later append. Rejecting
    // the append leaves the mailbox payload untouched and obliges the owner to
    // project and publish a current full snapshot.
    [[nodiscard]] AppendPublishResult publish_append(TextAppend append);
    [[nodiscard]] bool wait_for_written(std::chrono::milliseconds deadline);
    // Wakes a final-drain wait when the actor receives a shutdown reason that
    // must not spend the ordinary SSE drain interval. The interruption is
    // remembered so it also wins a race immediately before the wait begins.
    void interrupt_final_drain() noexcept;
    void close() noexcept;

private:
    void publish_snapshot_locked(SnapshotEvent snapshot);
    [[nodiscard]] AppendPublishResult publish_append_locked(TextAppend append);

    std::mutex mutex_;
    std::condition_variable changed_;
    bool closed_{};
    bool final_drain_interrupted_{};
    std::uint64_t active_stream_{};
    std::uint64_t next_stream_{1};
    std::shared_ptr<const SsePayload> in_flight_;
    std::shared_ptr<const SsePayload> pending_;
    std::optional<TextTarget> target_;
    std::uint64_t next_sequence_{};
    std::size_t collapsed_payloads_{};
};

} // namespace cha::web
