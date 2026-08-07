#include "ui/web/sse_mailbox.h"

#include <utility>

namespace cha::web {

SseMailbox::Stream SseMailbox::begin_stream(SnapshotEvent snapshot) {
    std::lock_guard lock(mutex_);
    const Stream stream{next_stream_++};
    active_stream_ = stream.id;
    in_flight_.reset();
    publish_snapshot_locked(std::move(snapshot));
    // The initial snapshot starts the new stream's accounting window. Reset
    // after publishing so correctness does not depend on pending_ being empty.
    collapsed_payloads_ = 0;
    changed_.notify_all();
    return stream;
}

SseMailbox::Next SseMailbox::next(
    Stream stream,
    std::chrono::milliseconds heartbeat_interval) {
    std::unique_lock lock(mutex_);
    changed_.wait_for(lock, heartbeat_interval, [&] {
        return closed_ || active_stream_ != stream.id
            || (!in_flight_ && pending_);
    });
    if (closed_ || active_stream_ != stream.id) return {};
    if (!in_flight_ && pending_) {
        in_flight_ = std::move(pending_);
        pending_.reset();
        return {.open = true, .payload = in_flight_};
    }
    // No payload is a writer heartbeat, not stream closure.
    return {.open = true};
}

void SseMailbox::written(Stream stream) noexcept {
    std::lock_guard lock(mutex_);
    if (active_stream_ != stream.id || !in_flight_) return;
    in_flight_.reset();
    changed_.notify_all();
}

std::size_t SseMailbox::end_stream(Stream stream) noexcept {
    std::lock_guard lock(mutex_);
    if (active_stream_ != stream.id) return 0;
    const std::size_t collapsed = collapsed_payloads_;
    active_stream_ = 0;
    in_flight_.reset();
    pending_.reset();
    changed_.notify_all();
    return collapsed;
}

void SseMailbox::publish(SnapshotEvent snapshot) {
    std::lock_guard lock(mutex_);
    if (!active_stream_ || closed_) return;
    publish_snapshot_locked(std::move(snapshot));
    changed_.notify_all();
}

void SseMailbox::publish_append(
    SessionTextAppend append,
    const SessionSnapshot& fallback_snapshot) {
    std::lock_guard lock(mutex_);
    if (!active_stream_ || closed_) return;
    publish_append_locked(std::move(append), fallback_snapshot);
    changed_.notify_all();
}

bool SseMailbox::wait_for_written(std::chrono::milliseconds deadline) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, deadline, [&] {
        // The owner calls this only after publishing its final state.  Pending
        // payloads may have replaced one another, so publish/write counters do
        // not describe whether the final state has reached the writer.
        return closed_ || !active_stream_ || (!in_flight_ && !pending_);
    });
}

void SseMailbox::close() noexcept {
    std::lock_guard lock(mutex_);
    closed_ = true;
    active_stream_ = 0;
    in_flight_.reset();
    pending_.reset();
    changed_.notify_all();
}

void SseMailbox::publish_snapshot_locked(SnapshotEvent snapshot) {
    if (pending_) ++collapsed_payloads_;
    const auto selection = snapshot_append_selection(snapshot.snapshot);
    target_ = selection
        ? std::optional<SessionTextTarget>{selection->target}
        : std::nullopt;
    next_sequence_ = 0;
    pending_ = std::make_shared<const SsePayload>(std::move(snapshot));
}

void SseMailbox::publish_append_locked(
    SessionTextAppend append,
    const SessionSnapshot& fallback_snapshot) {
    if (append.text.empty() || !target_ || *target_ != append.target) {
        publish_snapshot_locked({fallback_snapshot});
        return;
    }
    if (const auto* pending = pending_
            ? std::get_if<AppendEvent>(pending_.get())
            : nullptr) {
        if (pending->target == append.target) {
            ++collapsed_payloads_;
            pending_ = std::make_shared<const SsePayload>(AppendEvent{
                pending->target, pending->text + append.text, pending->seq});
            return;
        }
        publish_snapshot_locked({fallback_snapshot});
        return;
    }
    if (pending_) { // A pending snapshot must absorb the later owner state.
        publish_snapshot_locked({fallback_snapshot});
        return;
    }
    pending_ = std::make_shared<const SsePayload>(AppendEvent{
        std::move(append.target), std::move(append.text), next_sequence_++});
}

} // namespace cha::web
