#include "transcript/transcript.h"

#include <stdexcept>
#include <utility>

namespace cha {

TranscriptReadView::TranscriptReadView(const Transcript& transcript)
    : lock_(transcript.mutex_),
      entries_(&transcript.entries_),
      open_entry_id_(transcript.open_entry_id_),
      history_epoch_(transcript.history_epoch_) {
}

std::span<const TranscriptEntry> TranscriptReadView::entries() const noexcept {
    return *entries_;
}

std::optional<EntryId> TranscriptReadView::open_entry_id() const noexcept {
    return open_entry_id_;
}

std::size_t TranscriptReadView::history_epoch() const noexcept {
    return history_epoch_;
}

TranscriptEntry make_human_entry(
    EntryId id,
    ParticipantId addressed_to,
    std::string addressed_to_name,
    std::string text,
    std::optional<RequestId> request_id) {
    return {
        .id = id,
        .kind = EntryKind::human,
        .participant_id = std::string(human_participant_id),
        .display_name = std::string(human_display_name),
        .addressed_to = std::move(addressed_to),
        .addressed_to_name = std::move(addressed_to_name),
        .text = std::move(text),
        .status = EntryStatus::complete,
        .request_id = request_id,
    };
}

TranscriptEntry make_agent_entry(
    EntryId id,
    ParticipantId participant_id,
    std::string display_name,
    std::string text,
    EntryStatus status,
    std::optional<RequestId> request_id) {
    return {
        .id = id,
        .kind = EntryKind::agent,
        .participant_id = std::move(participant_id),
        .display_name = std::move(display_name),
        .text = std::move(text),
        .status = status,
        .request_id = request_id,
    };
}

TranscriptEntry make_notice_entry(EntryId id, std::string text) {
    return {
        .id = id,
        .kind = EntryKind::notice,
        .display_name = std::string(notice_display_name),
        .text = std::move(text),
    };
}

TranscriptEntry make_error_entry(
    EntryId id,
    std::string text,
    std::optional<RequestId> request_id,
    ParticipantId participant_id) {
    return {
        .id = id,
        .kind = EntryKind::error,
        .participant_id = std::move(participant_id),
        .display_name = std::string(error_display_name),
        .text = std::move(text),
        .status = EntryStatus::failed,
        .request_id = request_id,
    };
}

void validate_transcript_entry(const TranscriptEntry& entry) {
    if (entry.id == 0) {
        throw std::invalid_argument("Transcript entry ID must be positive");
    }
    if (entry.display_name.empty()) {
        throw std::invalid_argument("Transcript entry display name cannot be empty");
    }
    if ((entry.kind == EntryKind::human || entry.kind == EntryKind::agent)
        && entry.participant_id.empty()) {
        throw std::invalid_argument("Participant transcript entries require a participant ID");
    }
    if (entry.kind == EntryKind::human
        && (entry.addressed_to.empty() || entry.addressed_to_name.empty())) {
        throw std::invalid_argument("Human transcript entries require an addressed agent");
    }
    if (entry.kind != EntryKind::human
        && (!entry.addressed_to.empty() || !entry.addressed_to_name.empty())) {
        throw std::invalid_argument("Only human transcript entries may address an agent");
    }
    if (entry.kind == EntryKind::error && entry.status != EntryStatus::failed) {
        throw std::invalid_argument("Error entries require failed status");
    }
    if ((entry.kind == EntryKind::human || entry.kind == EntryKind::notice)
        && entry.status != EntryStatus::complete) {
        throw std::invalid_argument("Human and notice entries require complete status");
    }
    if (entry.kind == EntryKind::agent && entry.status == EntryStatus::failed) {
        throw std::invalid_argument("Agent entries cannot have failed status");
    }
    if (entry.kind == EntryKind::agent
        && entry.status == EntryStatus::complete
        && entry.text.empty()) {
        throw std::invalid_argument("A completed agent entry requires text content");
    }
    if (entry.kind == EntryKind::agent
        && entry.status == EntryStatus::cancelled
        && entry.text.empty()) {
        throw std::invalid_argument("A cancelled agent entry requires answer content");
    }
}

void require_terminal_transcript_entry(const TranscriptEntry& entry) {
    validate_transcript_entry(entry);
    if (entry.status == EntryStatus::streaming) {
        throw std::invalid_argument("A terminal transcript entry cannot have streaming status");
    }
}

void require_storable_transcript_entry(const TranscriptEntry& entry) {
    require_terminal_transcript_entry(entry);
}

void Transcript::add_entry(TranscriptEntry entry) {
    std::lock_guard lock(mutex_);
    if (open_entry_id_) {
        throw std::logic_error("Cannot add an entry while another entry is streaming");
    }
    require_terminal_transcript_entry(entry);
    require_next_id(entry.id);

    entries_.push_back(std::move(entry));
    ++revision_;
}

void Transcript::begin_entry(TranscriptEntry entry) {
    std::lock_guard lock(mutex_);
    if (open_entry_id_) {
        throw std::logic_error("A transcript entry is already streaming");
    }
    validate_transcript_entry(entry);
    if (entry.kind != EntryKind::agent || entry.status != EntryStatus::streaming) {
        throw std::invalid_argument("Only an agent entry with streaming status can be opened");
    }
    require_next_id(entry.id);

    open_entry_id_ = entry.id;
    entries_.push_back(std::move(entry));
    ++revision_;
}

void Transcript::append_answer(EntryId entry_id, std::string_view text) {
    std::lock_guard lock(mutex_);
    if (!open_entry_id_ || *open_entry_id_ != entry_id) {
        throw std::logic_error("The requested transcript entry is not streaming");
    }

    entries_.back().text.append(text);
    ++revision_;
}

void Transcript::finish_entry(EntryId entry_id, EntryStatus status) {
    std::lock_guard lock(mutex_);
    if (!open_entry_id_ || *open_entry_id_ != entry_id) {
        throw std::logic_error("The requested transcript entry is not streaming");
    }
    if (status != EntryStatus::complete && status != EntryStatus::cancelled) {
        throw std::invalid_argument("A finished agent entry requires complete or cancelled status");
    }
    if (status == EntryStatus::complete && entries_.back().text.empty()) {
        throw std::invalid_argument("A completed agent entry requires text content");
    }
    if (status == EntryStatus::cancelled
        && entries_.back().text.empty()) {
        throw std::invalid_argument("A cancelled agent entry requires answer content");
    }

    entries_.back().status = status;
    open_entry_id_.reset();
    ++revision_;
}

void Transcript::discard_entry(EntryId entry_id) {
    std::lock_guard lock(mutex_);
    if (!open_entry_id_ || *open_entry_id_ != entry_id) {
        throw std::logic_error("The requested transcript entry is not streaming");
    }

    entries_.pop_back();
    open_entry_id_.reset();
    ++revision_;
}

void Transcript::clear() {
    std::lock_guard lock(mutex_);
    if (open_entry_id_) {
        throw std::logic_error("Cannot clear a transcript while an entry is streaming");
    }
    entries_.clear();
    ++revision_;
    ++history_epoch_;
}

void Transcript::replace_entries(std::vector<TranscriptEntry> entries) {
    std::lock_guard lock(mutex_);
    if (open_entry_id_) {
        throw std::logic_error("Cannot replace entries while an entry is streaming");
    }
    EntryId previous_id = 0;
    for (const TranscriptEntry& entry : entries) {
        require_terminal_transcript_entry(entry);
        if (entry.id <= previous_id) {
            throw std::invalid_argument("Transcript entry IDs must be strictly increasing");
        }
        previous_id = entry.id;
    }

    entries_ = std::move(entries);
    ++revision_;
    ++history_epoch_;
}

TranscriptSnapshot Transcript::snapshot() const {
    std::lock_guard lock(mutex_);
    return {entries_, revision_, open_entry_id_, history_epoch_};
}

std::vector<TranscriptEntry> Transcript::entries() const {
    return snapshot().entries;
}

std::optional<EntryId> Transcript::open_entry_id() const {
    std::lock_guard lock(mutex_);
    return open_entry_id_;
}

TranscriptReadView Transcript::read() const {
    return TranscriptReadView(*this);
}

void Transcript::require_next_id(EntryId entry_id) const {
    if (!entries_.empty() && entry_id <= entries_.back().id) {
        throw std::invalid_argument("Transcript entry IDs must be strictly increasing");
    }
}

} // namespace cha
