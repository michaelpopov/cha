#include "web/sse_mailbox.h"

#include <stdexcept>
#include <utility>

namespace cha::web {

SseMailbox::SseMailbox(
    cha::app::SequencePolicy policy,
    std::size_t pending_append_byte_limit)
    : output_(std::make_shared<cha::app::SessionOutput>(
          policy, pending_append_byte_limit)) {}

SseMailbox::SseMailbox(std::shared_ptr<cha::app::SessionOutput> output)
    : output_(std::move(output)) {
    if (!output_) throw std::invalid_argument("SSE mailbox needs session output");
}

SsePayload SseMailbox::to_sse(const cha::app::SessionOutputItem& item) {
    if (item.kind == cha::app::SessionOutputItem::Kind::snapshot) {
        return SnapshotEvent{item.snapshot};
    }
    return AppendEvent{item.target, item.text, item.seq};
}

bool SseMailbox::stream_live(Stream stream) const {
    return !closed_ && active_stream_ == stream.id
        && output_->generation() == bound_generation_ && !output_->closed();
}

SseMailbox::Stream SseMailbox::begin_stream(SnapshotEvent snapshot) {
    std::lock_guard lock(mutex_);
    const Stream stream{next_stream_++};
    active_stream_ = stream.id;
    in_flight_.reset();
    closed_ = false;
    output_->attach();
    bound_generation_ = output_->generation();
    output_->publish_snapshot(std::move(snapshot.snapshot));
    output_->clear_collapsed();
    changed_.notify_all();
    return stream;
}

SseMailbox::Stream SseMailbox::listen() {
    std::lock_guard lock(mutex_);
    const Stream stream{next_stream_++};
    active_stream_ = stream.id;
    in_flight_.reset();
    closed_ = false;
    bound_generation_ = output_->generation();
    changed_.notify_all();
    return stream;
}

SseMailbox::Next SseMailbox::next(
    Stream stream,
    std::chrono::milliseconds heartbeat_interval) {
    output_->wait_for_work(heartbeat_interval);
    std::unique_lock lock(mutex_);
    if (!stream_live(stream)) {
        return {.ending = !closed_ && (active_stream_ > stream.id
            || output_->generation() != bound_generation_)
            ? Ending::superseded
            : Ending::closed};
    }
    if (!in_flight_ && output_->has_pending()) {
        const auto item = output_->take();
        if (item) {
            in_flight_ = std::make_shared<const SsePayload>(to_sse(*item));
            return {.open = true, .payload = in_flight_};
        }
    }
    return {.open = true};
}

void SseMailbox::written(Stream stream) noexcept {
    std::lock_guard lock(mutex_);
    if (!stream_live(stream) || !in_flight_) return;
    in_flight_.reset();
    output_->acknowledge();
    changed_.notify_all();
}

std::size_t SseMailbox::end_stream(Stream stream) noexcept {
    std::lock_guard lock(mutex_);
    if (active_stream_ != stream.id) return 0;
    if (output_->generation() != bound_generation_) {
        active_stream_ = 0;
        in_flight_.reset();
        changed_.notify_all();
        return 0;
    }
    const std::size_t collapsed = output_->collapsed_payloads();
    active_stream_ = 0;
    in_flight_.reset();
    output_->detach();
    changed_.notify_all();
    return collapsed;
}

void SseMailbox::publish(SnapshotEvent snapshot) {
    std::lock_guard lock(mutex_);
    if (closed_ || (!active_stream_ && !output_->attached())) return;
    output_->publish_snapshot(std::move(snapshot.snapshot));
    changed_.notify_all();
}

AppendPublishResult SseMailbox::publish_append(TextAppend append) {
    std::lock_guard lock(mutex_);
    if (closed_ || (!active_stream_ && !output_->attached())) {
        return AppendPublishResult::Accepted;
    }
    const AppendPublishResult result = output_->publish_append(std::move(append));
    if (result == AppendPublishResult::Accepted) changed_.notify_all();
    return result;
}

bool SseMailbox::wait_for_written(std::chrono::milliseconds deadline) {
    (void)output_->wait_until_consumed(deadline);
    std::lock_guard lock(mutex_);
    return closed_ || !active_stream_
        || (!in_flight_ && output_->idle());
}

void SseMailbox::interrupt_final_drain() noexcept {
    {
        std::lock_guard lock(mutex_);
        final_drain_interrupted_ = true;
    }
    output_->interrupt_wait();
    changed_.notify_all();
}

void SseMailbox::close() noexcept {
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
        active_stream_ = 0;
        in_flight_.reset();
    }
    output_->close();
    changed_.notify_all();
}

} // namespace cha::web
