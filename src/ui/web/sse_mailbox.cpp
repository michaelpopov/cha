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

AppendPublishResult SseMailbox::publish_append(TextAppend append) {
    std::lock_guard lock(mutex_);
    // With no live stream there is no browser state to repair; the next
    // connection starts from a full snapshot anyway.
    if (!active_stream_ || closed_) return AppendPublishResult::Accepted;
    const AppendPublishResult result = publish_append_locked(std::move(append));
    if (result == AppendPublishResult::Accepted) changed_.notify_all();
    return result;
}

bool SseMailbox::wait_for_written(std::chrono::milliseconds deadline) {
    std::unique_lock lock(mutex_);
    const auto written = [&] {
        // The owner calls this only after publishing its final state.  Pending
        // payloads may have replaced one another, so publish/write counters do
        // not describe whether the final state has reached the writer.
        return closed_ || !active_stream_ || (!in_flight_ && !pending_);
    };
    (void)changed_.wait_for(lock, deadline, [&] {
        return final_drain_interrupted_ || written();
    });
    return written();
}

void SseMailbox::interrupt_final_drain() noexcept {
    std::lock_guard lock(mutex_);
    final_drain_interrupted_ = true;
    changed_.notify_all();
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
        ? std::optional<TextTarget>{selection->target}
        : std::nullopt;
    next_sequence_ = 0;
    pending_ = std::make_shared<const SsePayload>(std::move(snapshot));
}

AppendPublishResult SseMailbox::publish_append_locked(TextAppend append) {
    // An empty append is a controller contract error. Reporting it as an
    // unrepresentable update keeps the browser correct either way.
    if (append.text.empty() || !target_ || *target_ != append.target) {
        return AppendPublishResult::SnapshotRequired;
    }
    if (const auto* pending = pending_
            ? std::get_if<AppendEvent>(pending_.get())
            : nullptr) {
        if (pending->target != append.target) {
            return AppendPublishResult::SnapshotRequired;
        }
        ++collapsed_payloads_;
        pending_ = std::make_shared<const SsePayload>(AppendEvent{
            pending->target, pending->text + append.text, pending->seq});
        return AppendPublishResult::Accepted;
    }
    // A pending snapshot must absorb the later owner state instead.
    if (pending_) return AppendPublishResult::SnapshotRequired;
    pending_ = std::make_shared<const SsePayload>(AppendEvent{
        std::move(append.target), std::move(append.text), next_sequence_++});
    return AppendPublishResult::Accepted;
}

} // namespace cha::web
