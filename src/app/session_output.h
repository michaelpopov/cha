#pragma once

#include "web/protocol.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace cha::app {

enum class SequencePolicy {
    // HTTP SSE: a snapshot resets append numbering. Snapshots themselves are
    // unnumbered on that wire.
    reset_on_snapshot,
    // Native: the initial snapshot is sequence zero; later snapshots and
    // appends share one increasing sequence.
    monotonic,
};

struct SessionOutputItem {
    enum class Kind { snapshot, append } kind{Kind::snapshot};
    std::uint64_t seq{};
    cha::web::SessionSnapshot snapshot;
    cha::TextTarget target;
    std::string text;
};

// Coalescing owner-to-consumer queue: compatible-append merge, snapshot
// fallback, and one in-flight payload. Sequence policy and byte bounds are
// applied when a payload is committed to pending.
class SessionOutput {
public:
    explicit SessionOutput(
        SequencePolicy policy = SequencePolicy::reset_on_snapshot,
        std::size_t pending_append_byte_limit = 65536);

    void attach();
    void detach() noexcept;
    [[nodiscard]] bool attached() const;
    [[nodiscard]] std::uint64_t generation() const;

    void publish_snapshot(cha::web::SessionSnapshot snapshot);
    [[nodiscard]] cha::web::AppendPublishResult publish_append(
        cha::TextAppend append);

    [[nodiscard]] std::shared_ptr<const SessionOutputItem> take();
    void acknowledge() noexcept;
    [[nodiscard]] bool has_in_flight() const;
    [[nodiscard]] bool has_pending() const;
    [[nodiscard]] bool idle() const;
    void wait_for_work(std::chrono::milliseconds timeout);
    [[nodiscard]] bool wait_until_consumed(std::chrono::milliseconds deadline);
    void interrupt_wait() noexcept;

    [[nodiscard]] std::size_t collapsed_payloads() const;
    void clear_collapsed() noexcept;
    [[nodiscard]] std::uint64_t next_sequence() const;

    // Marks the producer closed. Pending/in-flight payloads stay so a native
    // consumer can still take a terminal snapshot.
    void close() noexcept;
    [[nodiscard]] bool closed() const;

private:
    void publish_snapshot_locked(cha::web::SessionSnapshot snapshot);
    [[nodiscard]] cha::web::AppendPublishResult publish_append_locked(
        cha::TextAppend append);

    SequencePolicy policy_;
    std::size_t pending_append_byte_limit_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool attached_{};
    bool closed_{};
    bool interrupt_{};
    std::uint64_t generation_{};
    std::shared_ptr<const SessionOutputItem> in_flight_;
    std::shared_ptr<const SessionOutputItem> pending_;
    std::optional<cha::TextTarget> target_;
    std::uint64_t next_sequence_{};
    std::size_t collapsed_payloads_{};
};

} // namespace cha::app
