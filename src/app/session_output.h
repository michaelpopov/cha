#pragma once

#include "web/protocol.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace cha::app {

struct SessionOutputItem {
    enum class Kind { snapshot, append } kind{Kind::snapshot};
    std::uint64_t seq{};
    cha::web::SessionSnapshot snapshot;
    cha::TextTarget target;
    std::string text;
};

// Coalescing owner-to-consumer queue: compatible-append merge, snapshot
// fallback, and one in-flight payload. Sequence numbers are assigned only when
// the surviving pending payload is taken for delivery.
class SessionOutput {
public:
    explicit SessionOutput(
        std::size_t pending_append_byte_limit = 65536);

    void attach();
    void detach() noexcept;
    [[nodiscard]] bool attached() const;
    [[nodiscard]] std::uint64_t generation() const;

    void publish_snapshot(cha::web::SessionSnapshot snapshot);
    // Discard an obsolete pending payload without copying the transcript.
    // The owner materializes it once the in-flight delivery is acknowledged.
    void require_snapshot();
    [[nodiscard]] bool snapshot_needed() const;
    [[nodiscard]] cha::web::AppendPublishResult publish_append(
        cha::TextAppend append);

    [[nodiscard]] std::shared_ptr<const SessionOutputItem> take();
    void acknowledge() noexcept;
    [[nodiscard]] bool has_in_flight() const;
    [[nodiscard]] bool has_pending() const;
    [[nodiscard]] bool idle() const;

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

    std::size_t pending_append_byte_limit_;
    mutable std::mutex mutex_;
    bool attached_{};
    bool closed_{};
    bool dirty_{};
    bool requested_{};
    std::uint64_t generation_{};
    std::shared_ptr<const SessionOutputItem> in_flight_;
    std::shared_ptr<SessionOutputItem> pending_;
    std::optional<cha::TextTarget> target_;
    std::uint64_t next_sequence_{};
    std::size_t collapsed_payloads_{};
};

} // namespace cha::app
