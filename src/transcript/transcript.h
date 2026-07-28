#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
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

// Describes whether a transcript entry is final, being streamed, or was terminated.
enum class EntryStatus : std::int64_t {
    complete = 0,
    streaming = 1,
    cancelled = 2,
    failed = 3,
};

// The one transcript record every layer agrees on: rendering, persistence, and model-context
// projection all read this type. It keeps semantic kind and participant identity separate from
// the display label, records who a message was addressed to, and tracks whether the record is
// final or still being streamed.
struct TranscriptEntry {
    EntryId id{};
    EntryKind kind{EntryKind::notice};
    ParticipantId participant_id;
    std::string display_name;
    ParticipantId addressed_to;
    std::string addressed_to_name;
    std::string text;
    EntryStatus status{EntryStatus::complete};
    std::optional<RequestId> request_id;

    bool operator==(const TranscriptEntry&) const = default;
};

// A half-open range of transcript entry IDs excluded from model context. The
// begin-only state records an open span; membership requires both bounds.
struct OffrecordSpan {
    std::optional<EntryId> begin;
    std::optional<EntryId> end;

    [[nodiscard]] bool contains(EntryId id) const noexcept {
        return begin && end && *begin <= id && id < *end;
    }

    bool operator==(const OffrecordSpan&) const = default;
};

// An owning point-in-time copy of a Transcript. The revision and history epoch
// let a renderer decide what changed since the frame it drew last.
struct TranscriptSnapshot {
    std::vector<TranscriptEntry> entries;
    std::size_t revision{};
    std::optional<EntryId> open_entry_id;
    std::size_t history_epoch{};
};

// An owned, immutable source for model-context projection. Unlike the
// presentation-oriented TranscriptSnapshot, it includes the off-record span
// that determines which entries are visible to a completion.
struct CompletionHistory {
    std::vector<TranscriptEntry> entries;
    std::optional<EntryId> open_entry_id;
    OffrecordSpan offrecord_span;
};

TranscriptEntry make_human_entry(
    EntryId id,
    ParticipantId addressed_to,
    std::string addressed_to_name,
    std::string text,
    std::optional<RequestId> request_id = std::nullopt);
TranscriptEntry make_agent_entry(
    EntryId id,
    ParticipantId participant_id,
    std::string display_name,
    std::string text,
    EntryStatus status,
    std::optional<RequestId> request_id = std::nullopt);
TranscriptEntry make_notice_entry(EntryId id, std::string text);
TranscriptEntry make_hide_on_marker(EntryId id);
TranscriptEntry make_hide_marker(EntryId id);
TranscriptEntry make_hide_off_marker(EntryId id);
TranscriptEntry make_error_entry(
    EntryId id,
    std::string text,
    std::optional<RequestId> request_id = std::nullopt,
    ParticipantId participant_id = {});

// Validates the semantic fields and kind/status combination of one transcript entry.
void validate_transcript_entry(const TranscriptEntry& entry);

// Validates an entry and rejects active streaming state where a terminal record is required.
void require_terminal_transcript_entry(const TranscriptEntry& entry);

// Applies the terminal-entry contract at the persistence boundary.
void require_storable_transcript_entry(const TranscriptEntry& entry);

// The main-thread-owned live transcript of one session. It is not thread-safe:
// all reads and mutations must happen on its owning thread. It allows at most
// one open streaming entry and exposes only owning snapshots or narrow value
// accessors. It depends on nothing beyond the entry model declared above.
class Transcript {
public:
    Transcript() = default;
    Transcript(const Transcript&) = delete;
    Transcript& operator=(const Transcript&) = delete;
    Transcript(Transcript&&) = delete;
    Transcript& operator=(Transcript&&) = delete;

    void add_entry(TranscriptEntry entry);
    void begin_entry(TranscriptEntry entry);
    void append_answer(EntryId entry_id, std::string_view text);
    void finish_entry(EntryId entry_id, EntryStatus status);
    void discard_entry(EntryId entry_id);
    void clear();
    void replace_entries(std::vector<TranscriptEntry> entries);

    // Each successful mutation also appends its transient presentation marker.
    // A false result means the command precondition failed without mutation.
    [[nodiscard]] bool open_offrecord(EntryId marker_id);
    [[nodiscard]] bool extend_offrecord(EntryId marker_id);
    [[nodiscard]] bool restore_offrecord(EntryId marker_id);

    TranscriptSnapshot snapshot() const;
    CompletionHistory completion_history() const;
    std::vector<TranscriptEntry> entries() const;
    std::optional<EntryId> open_entry_id() const;
    std::string open_entry_text(EntryId entry_id) const;
    OffrecordSpan offrecord_span() const;

private:
    void require_next_id(EntryId entry_id) const;
    EntryId boundary() const;

    std::vector<TranscriptEntry> entries_;
    std::size_t revision_{};
    std::optional<EntryId> open_entry_id_;
    OffrecordSpan offrecord_;
    std::size_t history_epoch_{};
};

} // namespace cha
