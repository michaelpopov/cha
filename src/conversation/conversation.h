#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

using RequestId = std::uint64_t;
using EntryId = std::uint64_t;
using ParticipantId = std::string;

inline constexpr std::string_view human_participant_id = "human";
inline constexpr std::string_view human_display_name = "You";
inline constexpr std::string_view notice_display_name = "System";
inline constexpr std::string_view error_display_name = "Error";

// Classifies transcript semantics independently of the label rendered to the user.
enum class EntryKind : std::int64_t {
    human = 0,
    agent = 1,
    notice = 2,
    error = 3,
};

// Describes whether an entry is final or represents an active or terminated completion.
enum class CompletionStatus : std::int64_t {
    complete = 0,
    streaming = 1,
    cancelled = 2,
    failed = 3,
};

// Classifies one streamed completion fragment as reasoning or answer text.
enum class CompletionDeltaKind {
    reasoning,
    answer,
};

// Carries one semantic transport fragment without attaching request identity.
struct CompletionDelta {
    CompletionDeltaKind kind{CompletionDeltaKind::answer};
    std::string text;
};

// Stores one typed transcript record for rendering, persistence, and context projection.
struct ConversationEntry {
    EntryId id{};
    EntryKind kind{EntryKind::notice};
    ParticipantId participant_id;
    std::string display_name;
    ParticipantId addressed_to;
    std::string addressed_to_name;
    std::string reasoning_text;
    std::string text;
    CompletionStatus status{CompletionStatus::complete};
    std::optional<RequestId> request_id;

    bool operator==(const ConversationEntry&) const = default;
};

struct AgentResponseText {
    std::string reasoning;
    std::string answer;
};

// Provides a consistent read-only copy of conversation state for concurrent consumers.
struct ConversationSnapshot {
    std::vector<ConversationEntry> entries;
    std::size_t revision{};
    std::optional<EntryId> open_entry_id;
    std::size_t history_epoch{};
};

class Conversation;

// Holds the conversation mutex while exposing a non-owning, consistent read view.
// Spans and references obtained from this object are valid only while it is alive.
class ConversationReadView {
public:
    ~ConversationReadView() = default;

    ConversationReadView(const ConversationReadView&) = delete;
    ConversationReadView& operator=(const ConversationReadView&) = delete;
    ConversationReadView(ConversationReadView&&) = delete;
    ConversationReadView& operator=(ConversationReadView&&) = delete;

    std::span<const ConversationEntry> entries() const noexcept;
    std::size_t revision() const noexcept;
    std::optional<EntryId> open_entry_id() const noexcept;
    std::size_t history_epoch() const noexcept;

private:
    friend class Conversation;

    explicit ConversationReadView(const Conversation& conversation);

    std::unique_lock<std::mutex> lock_;
    const std::vector<ConversationEntry>* entries_{};
    std::optional<EntryId> open_entry_id_;
    std::size_t history_epoch_{};
};

ConversationEntry make_human_entry(
    EntryId id,
    ParticipantId addressed_to,
    std::string addressed_to_name,
    std::string text,
    std::optional<RequestId> request_id = std::nullopt);
ConversationEntry make_agent_entry(
    EntryId id,
    ParticipantId participant_id,
    std::string display_name,
    AgentResponseText response,
    CompletionStatus status,
    std::optional<RequestId> request_id = std::nullopt);
ConversationEntry make_agent_entry(
    EntryId id,
    ParticipantId participant_id,
    std::string display_name,
    std::string text,
    CompletionStatus status,
    std::optional<RequestId> request_id = std::nullopt);
ConversationEntry make_notice_entry(EntryId id, std::string text);
ConversationEntry make_error_entry(
    EntryId id,
    std::string text,
    std::optional<RequestId> request_id = std::nullopt,
    ParticipantId participant_id = {});

// Validates the semantic fields and kind/status combination of one transcript entry.
void validate_conversation_entry(const ConversationEntry& entry);

// Validates an entry and rejects active streaming state where a terminal record is required.
void require_terminal_conversation_entry(const ConversationEntry& entry);

// Validates that an entry is terminal and contains no ephemeral reasoning.
void require_storable_conversation_entry(const ConversationEntry& entry);

// Keeps a thread-safe typed transcript with at most one coordinator-owned streaming entry.
class Conversation {
public:
    Conversation() = default;
    Conversation(const Conversation&) = delete;
    Conversation& operator=(const Conversation&) = delete;
    Conversation(Conversation&&) = delete;
    Conversation& operator=(Conversation&&) = delete;

    void add_entry(ConversationEntry entry);
    void begin_entry(ConversationEntry entry);
    void append_to_entry(
        EntryId entry_id,
        CompletionDeltaKind kind,
        std::string_view text);
    void finish_entry(EntryId entry_id, CompletionStatus status);
    void discard_entry(EntryId entry_id);
    void clear();
    void replace_entries(std::vector<ConversationEntry> entries);

    ConversationSnapshot snapshot() const;
    std::vector<ConversationEntry> entries() const;
    std::optional<EntryId> open_entry_id() const;
    ConversationReadView read() const;

private:
    friend class ConversationReadView;
    void require_next_id(EntryId entry_id) const;

    mutable std::mutex mutex_;
    std::vector<ConversationEntry> entries_;
    std::size_t revision_{};
    std::optional<EntryId> open_entry_id_;
    std::size_t history_epoch_{};
};

} // namespace cha
