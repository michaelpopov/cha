#include "app/session_output.h"

#include <utility>

namespace cha::app {

SessionOutput::SessionOutput(
    std::size_t pending_append_byte_limit)
    : pending_append_byte_limit_(pending_append_byte_limit) {}

void SessionOutput::attach() {
    std::lock_guard lock(mutex_);
    attached_ = true;
    closed_ = false;
    dirty_ = false;
    requested_ = true;
    ++generation_;
    in_flight_.reset();
    pending_.reset();
    target_.reset();
    next_sequence_ = 0;
}

void SessionOutput::detach() noexcept {
    std::lock_guard lock(mutex_);
    attached_ = false;
    dirty_ = false;
    in_flight_.reset();
    pending_.reset();
}

bool SessionOutput::attached() const {
    std::lock_guard lock(mutex_);
    return attached_ && !closed_;
}

std::uint64_t SessionOutput::generation() const {
    std::lock_guard lock(mutex_);
    return generation_;
}

void SessionOutput::publish_snapshot(SessionSnapshot snapshot) {
    std::lock_guard lock(mutex_);
    if (!attached_ || closed_) return;
    publish_snapshot_locked(std::move(snapshot));
}

void SessionOutput::require_snapshot() {
    std::lock_guard lock(mutex_);
    if (!attached_ || closed_) return;
    if (pending_ || dirty_) ++collapsed_payloads_;
    pending_.reset();
    dirty_ = true;
}

bool SessionOutput::snapshot_needed() const {
    std::lock_guard lock(mutex_);
    return attached_ && !closed_ && dirty_ && requested_ && !in_flight_;
}

AppendPublishResult SessionOutput::publish_append(
    TextAppend append) {
    std::lock_guard lock(mutex_);
    if (!attached_ || closed_ || dirty_) {
        return AppendPublishResult::Accepted;
    }
    const auto result = publish_append_locked(std::move(append));
    return result;
}

std::shared_ptr<const SessionOutputItem> SessionOutput::take() {
    std::lock_guard lock(mutex_);
    if (in_flight_) return {};
    requested_ = true;
    if (!pending_) return {};
    auto committed = std::move(pending_);
    committed->seq = next_sequence_++;
    in_flight_ = std::move(committed);
    requested_ = false;
    return in_flight_;
}

void SessionOutput::acknowledge() noexcept {
    std::lock_guard lock(mutex_);
    in_flight_.reset();
}

bool SessionOutput::has_in_flight() const {
    std::lock_guard lock(mutex_);
    return static_cast<bool>(in_flight_);
}

bool SessionOutput::has_pending() const {
    std::lock_guard lock(mutex_);
    return pending_ || dirty_;
}

bool SessionOutput::idle() const {
    std::lock_guard lock(mutex_);
    return !in_flight_ && !pending_ && !dirty_;
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
}

bool SessionOutput::closed() const {
    std::lock_guard lock(mutex_);
    return closed_;
}

void SessionOutput::publish_snapshot_locked(SessionSnapshot snapshot) {
    dirty_ = false;
    requested_ = false;
    if (pending_) ++collapsed_payloads_;
    const auto selection = snapshot_append_selection(snapshot);
    target_ = selection
        ? std::optional<TextTarget>{selection->target}
        : std::nullopt;
    auto item = std::make_shared<SessionOutputItem>();
    item->kind = SessionOutputItem::Kind::snapshot;
    item->snapshot = std::move(snapshot);
    pending_ = std::move(item);
}

AppendPublishResult SessionOutput::publish_append_locked(
    TextAppend append) {
    if (append.text.empty() || !target_ || *target_ != append.target) {
        return AppendPublishResult::SnapshotRequired;
    }
    if (append.text.size() > pending_append_byte_limit_) {
        return AppendPublishResult::SnapshotRequired;
    }
    if (pending_ && pending_->kind == SessionOutputItem::Kind::append) {
        if (pending_->target != append.target) {
            return AppendPublishResult::SnapshotRequired;
        }
        if (pending_->text.size() + append.text.size()
            > pending_append_byte_limit_) {
            return AppendPublishResult::SnapshotRequired;
        }
        ++collapsed_payloads_;
        auto merged = std::make_shared<SessionOutputItem>(*pending_);
        merged->text += append.text;
        pending_ = std::move(merged);
        return AppendPublishResult::Accepted;
    }
    if (pending_) return AppendPublishResult::SnapshotRequired;
    auto item = std::make_shared<SessionOutputItem>();
    item->kind = SessionOutputItem::Kind::append;
    item->target = std::move(append.target);
    item->text = std::move(append.text);
    pending_ = std::move(item);
    return AppendPublishResult::Accepted;
}

} // namespace cha::app
