#include "conversation.h"

#include <stdexcept>
#include <utility>

namespace cha {

ConversationReadView::ConversationReadView(const Conversation& conversation)
    : lock_(conversation.mutex_),
      entries_(&conversation.entries_),
      revision_(conversation.revision_),
      open_entry_id_(conversation.open_entry_id_),
      history_epoch_(conversation.history_epoch_) {
}

std::span<const ConversationEntry> ConversationReadView::entries() const noexcept {
    return *entries_;
}

std::size_t ConversationReadView::revision() const noexcept {
    return revision_;
}

std::optional<EntryId> ConversationReadView::open_entry_id() const noexcept {
    return open_entry_id_;
}

std::size_t ConversationReadView::history_epoch() const noexcept {
    return history_epoch_;
}

ConversationEntry make_human_entry(
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
        .status = CompletionStatus::complete,
        .request_id = request_id,
    };
}

ConversationEntry make_agent_entry(
    EntryId id,
    ParticipantId participant_id,
    std::string display_name,
    std::string text,
    CompletionStatus status,
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

ConversationEntry make_notice_entry(EntryId id, std::string text) {
    return {
        .id = id,
        .kind = EntryKind::notice,
        .display_name = std::string(notice_display_name),
        .text = std::move(text),
    };
}

ConversationEntry make_error_entry(
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
        .status = CompletionStatus::failed,
        .request_id = request_id,
    };
}

void validate_conversation_entry(const ConversationEntry& entry) {
    if (entry.id == 0) {
        throw std::invalid_argument("Conversation entry ID must be positive");
    }
    if (entry.display_name.empty()) {
        throw std::invalid_argument("Conversation entry display name cannot be empty");
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
    if (entry.kind == EntryKind::error && entry.status != CompletionStatus::failed) {
        throw std::invalid_argument("Error entries require failed status");
    }
    if ((entry.kind == EntryKind::human || entry.kind == EntryKind::notice)
        && entry.status != CompletionStatus::complete) {
        throw std::invalid_argument("Human and notice entries require complete status");
    }
    if (entry.kind == EntryKind::agent && entry.status == CompletionStatus::failed) {
        throw std::invalid_argument("Agent entries cannot have failed status");
    }
    if (entry.kind == EntryKind::agent
        && entry.status == CompletionStatus::complete
        && entry.text.empty()) {
        throw std::invalid_argument("A completed agent entry requires text content");
    }
}

void require_terminal_conversation_entry(const ConversationEntry& entry) {
    validate_conversation_entry(entry);
    if (entry.status == CompletionStatus::streaming) {
        throw std::invalid_argument("A terminal conversation entry cannot have streaming status");
    }
}

void Conversation::add_entry(ConversationEntry entry) {
    std::lock_guard lock(mutex_);
    if (open_entry_id_) {
        throw std::logic_error("Cannot add an entry while another entry is streaming");
    }
    require_terminal_conversation_entry(entry);
    require_next_id(entry.id);

    entries_.push_back(std::move(entry));
    ++revision_;
}

void Conversation::begin_entry(ConversationEntry entry) {
    std::lock_guard lock(mutex_);
    if (open_entry_id_) {
        throw std::logic_error("A conversation entry is already streaming");
    }
    validate_conversation_entry(entry);
    if (entry.kind != EntryKind::agent || entry.status != CompletionStatus::streaming) {
        throw std::invalid_argument("Only an agent entry with streaming status can be opened");
    }
    require_next_id(entry.id);

    open_entry_id_ = entry.id;
    entries_.push_back(std::move(entry));
    ++revision_;
}

void Conversation::append_to_entry(EntryId entry_id, std::string_view text) {
    std::lock_guard lock(mutex_);
    if (!open_entry_id_ || *open_entry_id_ != entry_id) {
        throw std::logic_error("The requested conversation entry is not streaming");
    }

    entries_.back().text.append(text);
    ++revision_;
}

void Conversation::finish_entry(EntryId entry_id, CompletionStatus status) {
    std::lock_guard lock(mutex_);
    if (!open_entry_id_ || *open_entry_id_ != entry_id) {
        throw std::logic_error("The requested conversation entry is not streaming");
    }
    if (status != CompletionStatus::complete && status != CompletionStatus::cancelled) {
        throw std::invalid_argument("A finished agent entry requires complete or cancelled status");
    }
    if (status == CompletionStatus::complete && entries_.back().text.empty()) {
        throw std::invalid_argument("A completed agent entry requires text content");
    }

    entries_.back().status = status;
    open_entry_id_.reset();
    ++revision_;
}

void Conversation::discard_entry(EntryId entry_id) {
    std::lock_guard lock(mutex_);
    if (!open_entry_id_ || *open_entry_id_ != entry_id) {
        throw std::logic_error("The requested conversation entry is not streaming");
    }

    entries_.pop_back();
    open_entry_id_.reset();
    ++revision_;
}

void Conversation::clear() {
    std::lock_guard lock(mutex_);
    if (open_entry_id_) {
        throw std::logic_error("Cannot clear a conversation while an entry is streaming");
    }
    entries_.clear();
    ++revision_;
    ++history_epoch_;
}

void Conversation::replace_entries(std::vector<ConversationEntry> entries) {
    std::lock_guard lock(mutex_);
    if (open_entry_id_) {
        throw std::logic_error("Cannot replace entries while an entry is streaming");
    }
    EntryId previous_id = 0;
    for (const ConversationEntry& entry : entries) {
        require_terminal_conversation_entry(entry);
        if (entry.id <= previous_id) {
            throw std::invalid_argument("Conversation entry IDs must be strictly increasing");
        }
        previous_id = entry.id;
    }

    entries_ = std::move(entries);
    ++revision_;
    ++history_epoch_;
}

ConversationSnapshot Conversation::snapshot() const {
    std::lock_guard lock(mutex_);
    return {entries_, revision_, open_entry_id_, history_epoch_};
}

std::vector<ConversationEntry> Conversation::entries() const {
    return snapshot().entries;
}

std::size_t Conversation::revision() const {
    std::lock_guard lock(mutex_);
    return revision_;
}

std::optional<EntryId> Conversation::open_entry_id() const {
    std::lock_guard lock(mutex_);
    return open_entry_id_;
}

ConversationReadView Conversation::read() const {
    return ConversationReadView(*this);
}

void Conversation::require_next_id(EntryId entry_id) const {
    if (!entries_.empty() && entry_id <= entries_.back().id) {
        throw std::invalid_argument("Conversation entry IDs must be strictly increasing");
    }
}

} // namespace cha
