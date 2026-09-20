#include "app/session_output.h"

#include <utility>

namespace cha::app {

SessionOutput::SessionOutput(
    SequencePolicy policy,
    std::size_t pending_append_byte_limit)
    : policy_(policy),
      pending_append_byte_limit_(pending_append_byte_limit) {}

void SessionOutput::attach() {
    std::lock_guard lock(mutex_);
    attached_ = true;
    closed_ = false;
    interrupt_ = false;
    ++generation_;
    in_flight_.reset();
    pending_.reset();
    target_.reset();
    next_sequence_ = 0;
    changed_.notify_all();
}

void SessionOutput::detach() noexcept {
    std::lock_guard lock(mutex_);
    attached_ = false;
    in_flight_.reset();
    pending_.reset();
    changed_.notify_all();
}

bool SessionOutput::attached() const {
    std::lock_guard lock(mutex_);
    return attached_ && !closed_;
}

std::uint64_t SessionOutput::generation() const {
    std::lock_guard lock(mutex_);
    return generation_;
}

void SessionOutput::publish_snapshot(cha::web::SessionSnapshot snapshot) {
    std::lock_guard lock(mutex_);
    if (!attached_ || closed_) return;
    publish_snapshot_locked(std::move(snapshot));
    changed_.notify_all();
}

cha::web::AppendPublishResult SessionOutput::publish_append(
    cha::TextAppend append) {
    std::lock_guard lock(mutex_);
    if (!attached_ || closed_) {
        return cha::web::AppendPublishResult::Accepted;
    }
    const auto result = publish_append_locked(std::move(append));
    if (result == cha::web::AppendPublishResult::Accepted) changed_.notify_all();
    return result;
}

std::shared_ptr<const SessionOutputItem> SessionOutput::take() {
    std::lock_guard lock(mutex_);
    if (in_flight_ || !pending_) return {};
    auto committed = std::move(pending_);
    if (policy_ == SequencePolicy::reset_on_snapshot
        && committed->kind == SessionOutputItem::Kind::snapshot) {
        next_sequence_ = 0;
        committed->seq = 0;
    } else {
        committed->seq = next_sequence_++;
    }
    in_flight_ = std::move(committed);
    changed_.notify_all();
    return in_flight_;
}

void SessionOutput::acknowledge() noexcept {
    std::lock_guard lock(mutex_);
    in_flight_.reset();
    changed_.notify_all();
}

bool SessionOutput::has_in_flight() const {
    std::lock_guard lock(mutex_);
    return static_cast<bool>(in_flight_);
}

bool SessionOutput::has_pending() const {
    std::lock_guard lock(mutex_);
    return static_cast<bool>(pending_);
}

bool SessionOutput::idle() const {
    std::lock_guard lock(mutex_);
    return !in_flight_ && !pending_;
}

void SessionOutput::wait_for_work(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    changed_.wait_for(lock, timeout, [this] {
        return closed_ || interrupt_ || pending_ || !attached_;
    });
}

bool SessionOutput::wait_until_consumed(std::chrono::milliseconds deadline) {
    std::unique_lock lock(mutex_);
    const auto consumed = [this] {
        return closed_ || (!in_flight_ && !pending_);
    };
    (void)changed_.wait_for(lock, deadline, [this, &consumed] {
        return interrupt_ || consumed();
    });
    return consumed();
}

void SessionOutput::interrupt_wait() noexcept {
    std::lock_guard lock(mutex_);
    interrupt_ = true;
    changed_.notify_all();
}

std::size_t SessionOutput::collapsed_payloads() const {
    std::lock_guard lock(mutex_);
    return collapsed_payloads_;
}

void SessionOutput::clear_collapsed() noexcept {
    std::lock_guard lock(mutex_);
    collapsed_payloads_ = 0;
}

std::uint64_t SessionOutput::next_sequence() const {
    std::lock_guard lock(mutex_);
    return next_sequence_;
}

void SessionOutput::close() noexcept {
    std::lock_guard lock(mutex_);
    closed_ = true;
    attached_ = false;
    changed_.notify_all();
}

bool SessionOutput::closed() const {
    std::lock_guard lock(mutex_);
    return closed_;
}

void SessionOutput::publish_snapshot_locked(cha::web::SessionSnapshot snapshot) {
    if (pending_) ++collapsed_payloads_;
    const auto selection = cha::web::snapshot_append_selection(snapshot);
    target_ = selection
        ? std::optional<cha::TextTarget>{selection->target}
        : std::nullopt;
    auto item = std::make_shared<SessionOutputItem>();
    item->kind = SessionOutputItem::Kind::snapshot;
    item->snapshot = std::move(snapshot);
    pending_ = std::move(item);
}

cha::web::AppendPublishResult SessionOutput::publish_append_locked(
    cha::TextAppend append) {
    if (append.text.empty() || !target_ || *target_ != append.target) {
        return cha::web::AppendPublishResult::SnapshotRequired;
    }
    if (append.text.size() > pending_append_byte_limit_) {
        return cha::web::AppendPublishResult::SnapshotRequired;
    }
    if (pending_ && pending_->kind == SessionOutputItem::Kind::append) {
        if (pending_->target != append.target) {
            return cha::web::AppendPublishResult::SnapshotRequired;
        }
        if (pending_->text.size() + append.text.size()
            > pending_append_byte_limit_) {
            return cha::web::AppendPublishResult::SnapshotRequired;
        }
        ++collapsed_payloads_;
        auto merged = std::make_shared<SessionOutputItem>(*pending_);
        merged->text += append.text;
        pending_ = std::move(merged);
        return cha::web::AppendPublishResult::Accepted;
    }
    if (pending_) return cha::web::AppendPublishResult::SnapshotRequired;
    auto item = std::make_shared<SessionOutputItem>();
    item->kind = SessionOutputItem::Kind::append;
    item->target = std::move(append.target);
    item->text = std::move(append.text);
    pending_ = std::move(item);
    return cha::web::AppendPublishResult::Accepted;
}

} // namespace cha::app
