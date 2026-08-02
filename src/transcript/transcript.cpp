#include "transcript/transcript.h"

#include <stdexcept>
#include <utility>

namespace cha {

TranscriptEntry make_human_entry(HumanEntrySpec spec) {
    return {
        .id = spec.id,
        .kind = EntryKind::human,
        .participant_id = std::move(spec.author.id),
        .display_name = std::move(spec.author.display_name),
        .addressed_to = std::move(spec.addressed_to.id),
        .addressed_to_name = std::move(spec.addressed_to.display_name),
        .text = std::move(spec.text),
        .status = EntryStatus::complete,
        .request_id = spec.request_id,
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

namespace {

TranscriptEntry make_marker_entry(EntryId id, std::string display_name) {
    return {
        .id = id,
        .kind = EntryKind::notice,
        .display_name = std::move(display_name),
    };
}

} // namespace

TranscriptEntry make_hide_on_marker(EntryId id) {
    return make_marker_entry(id, "hide-on");
}

TranscriptEntry make_hide_marker(EntryId id) {
    return make_marker_entry(id, "hide");
}

TranscriptEntry make_hide_off_marker(EntryId id) {
    return make_marker_entry(id, "hide-off");
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
    if (open_entry_id_) {
        throw std::logic_error("Cannot add an entry while another entry is streaming");
    }
    require_terminal_transcript_entry(entry);
    require_next_id(entry.id);

    entries_.push_back(std::move(entry));
    ++revision_;
}

void Transcript::begin_entry(TranscriptEntry entry) {
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
    if (!open_entry_id_ || *open_entry_id_ != entry_id) {
        throw std::logic_error("The requested transcript entry is not streaming");
    }

    entries_.back().text.append(text);
    ++revision_;
}

void Transcript::finish_entry(EntryId entry_id, EntryStatus status) {
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
    if (!open_entry_id_ || *open_entry_id_ != entry_id) {
        throw std::logic_error("The requested transcript entry is not streaming");
    }

    entries_.pop_back();
    open_entry_id_.reset();
    ++revision_;
}

void Transcript::clear() {
    if (open_entry_id_) {
        throw std::logic_error("Cannot clear a transcript while an entry is streaming");
    }
    entries_.clear();
    offrecord_ = {};
    ++revision_;
    ++history_epoch_;
}

void Transcript::replace_entries(std::vector<TranscriptEntry> entries) {
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
    offrecord_ = {};
    ++revision_;
    ++history_epoch_;
}

bool Transcript::open_offrecord(EntryId marker_id) {
    if (open_entry_id_ || offrecord_.begin) {
        return false;
    }
    TranscriptEntry marker = make_hide_on_marker(marker_id);
    require_terminal_transcript_entry(marker);
    require_next_id(marker.id);
    offrecord_.begin = boundary();
    entries_.push_back(std::move(marker));
    ++revision_;
    return true;
}

bool Transcript::extend_offrecord(EntryId marker_id) {
    if (open_entry_id_ || !offrecord_.begin) {
        return false;
    }
    TranscriptEntry marker = make_hide_marker(marker_id);
    require_terminal_transcript_entry(marker);
    require_next_id(marker.id);
    offrecord_.end = boundary();
    entries_.push_back(std::move(marker));
    ++revision_;
    return true;
}

bool Transcript::restore_offrecord(EntryId marker_id) {
    if (open_entry_id_ || !offrecord_.begin) {
        return false;
    }
    TranscriptEntry marker = make_hide_off_marker(marker_id);
    require_terminal_transcript_entry(marker);
    require_next_id(marker.id);
    offrecord_ = {};
    entries_.push_back(std::move(marker));
    ++revision_;
    return true;
}

TranscriptView Transcript::view() const noexcept {
    return {entries_, revision_, open_entry_id_, history_epoch_};
}

std::size_t Transcript::size() const noexcept {
    return entries_.size();
}

CompletionHistory Transcript::completion_history() const {
    return {
        .entries = entries_,
        .open_entry_id = open_entry_id_,
        .offrecord_span = offrecord_,
    };
}

std::span<const TranscriptEntry> Transcript::entries() const noexcept {
    return entries_;
}

std::optional<EntryId> Transcript::open_entry_id() const {
    return open_entry_id_;
}

std::string Transcript::open_entry_text(EntryId entry_id) const {
    if (!open_entry_id_ || *open_entry_id_ != entry_id
        || entries_.empty() || entries_.back().id != entry_id
        || entries_.back().kind != EntryKind::agent
        || entries_.back().status != EntryStatus::streaming) {
        throw std::logic_error(
            "The requested transcript entry is not the open tail entry");
    }
    return entries_.back().text;
}

OffrecordSpan Transcript::offrecord_span() const {
    return offrecord_;
}

void Transcript::require_next_id(EntryId entry_id) const {
    if (!entries_.empty() && entry_id <= entries_.back().id) {
        throw std::invalid_argument("Transcript entry IDs must be strictly increasing");
    }
}

EntryId Transcript::boundary() const {
    return entries_.empty() ? 1 : entries_.back().id + 1;
}

} // namespace cha
