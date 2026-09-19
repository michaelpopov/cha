#pragma once

#include "app/session_output.h"
#include "web/protocol.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <variant>

namespace cha::web {

using SsePayload = std::variant<SnapshotEvent, AppendEvent>;

// HTTP SSE adapter over SessionOutput: stream takeover, heartbeat waits, and
// final-drain signalling. Merge/snapshot/one-in-flight live in SessionOutput.
class SseMailbox final {
public:
    explicit SseMailbox(
        cha::app::SequencePolicy policy = cha::app::SequencePolicy::reset_on_snapshot,
        std::size_t pending_append_byte_limit = 65536);
    explicit SseMailbox(std::shared_ptr<cha::app::SessionOutput> output);
    using Stream = SseStreamToken;
    enum class Ending {
        closed,
        superseded,
    };
    struct Next {
        bool open{};
        Ending ending{Ending::closed};
        std::shared_ptr<const SsePayload> payload;
    };

    [[nodiscard]] Stream begin_stream(SnapshotEvent snapshot);
    // Adopt an already-attached SessionOutput (owner published the snapshot).
    [[nodiscard]] Stream listen();
    [[nodiscard]] Next next(
        Stream stream,
        std::chrono::milliseconds heartbeat_interval);
    void written(Stream stream) noexcept;
    std::size_t end_stream(Stream stream) noexcept;

    void publish(SnapshotEvent snapshot);
    [[nodiscard]] AppendPublishResult publish_append(TextAppend append);
    [[nodiscard]] bool wait_for_written(std::chrono::milliseconds deadline);
    void interrupt_final_drain() noexcept;
    void close() noexcept;

private:
    [[nodiscard]] static SsePayload to_sse(const cha::app::SessionOutputItem& item);
    [[nodiscard]] bool stream_live(Stream stream) const;

    std::mutex mutex_;
    std::condition_variable changed_;
    bool closed_{};
    bool final_drain_interrupted_{};
    std::uint64_t active_stream_{};
    std::uint64_t next_stream_{1};
    std::uint64_t bound_generation_{};
    std::shared_ptr<cha::app::SessionOutput> output_;
    std::shared_ptr<const SsePayload> in_flight_;
};

} // namespace cha::web
