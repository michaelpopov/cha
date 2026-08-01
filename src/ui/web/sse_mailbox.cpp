#include "ui/web/sse_mailbox.h"

#include <utility>

namespace cha::web {
namespace {

bool same_target(const AppendTarget& left, const AppendTarget& right) {
    if (left.index() != right.index()) return false;
    if (const auto* entry = std::get_if<AppendTargetEntry>(&left)) {
        return entry->entry_id == std::get<AppendTargetEntry>(right).entry_id;
    }
    return std::get<AppendTargetReasoning>(left).request_id
        == std::get<AppendTargetReasoning>(right).request_id;
}

} // namespace

SseMailbox::Stream SseMailbox::begin_stream(SnapshotEvent snapshot) {
    std::lock_guard lock(mutex_);
    const Stream stream{next_stream_++};
    active_stream_ = stream.id;
    in_flight_.reset();
    publish_snapshot_locked(std::move(snapshot));
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

void SseMailbox::end_stream(Stream stream) noexcept {
    std::lock_guard lock(mutex_);
    if (active_stream_ != stream.id) return;
    active_stream_ = 0;
    in_flight_.reset();
    pending_.reset();
    changed_.notify_all();
}

void SseMailbox::publish(SnapshotEvent snapshot) {
    std::lock_guard lock(mutex_);
    if (!active_stream_ || closed_) return;
    publish_snapshot_locked(std::move(snapshot));
    changed_.notify_all();
}

void SseMailbox::publish_append(
    WebAppendCandidate candidate,
    const SessionSnapshot& fallback_snapshot) {
    std::lock_guard lock(mutex_);
    if (!active_stream_ || closed_) return;
    publish_append_locked(std::move(candidate), fallback_snapshot);
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
    target_ = snapshot_append_target(snapshot.snapshot);
    next_sequence_ = 0;
    pending_ = std::make_shared<const SsePayload>(std::move(snapshot));
}

void SseMailbox::publish_append_locked(
    WebAppendCandidate candidate,
    const SessionSnapshot& fallback_snapshot) {
    if (candidate.text.empty() || !target_ || !same_target(*target_, candidate.target)) {
        publish_snapshot_locked({fallback_snapshot});
        return;
    }
    if (const auto* pending = pending_
            ? std::get_if<AppendEvent>(pending_.get())
            : nullptr) {
        if (same_target(pending->target, candidate.target)) {
            pending_ = std::make_shared<const SsePayload>(AppendEvent{
                pending->target, pending->text + candidate.text, pending->seq});
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
        std::move(candidate.target), std::move(candidate.text), next_sequence_++});
}

} // namespace cha::web
