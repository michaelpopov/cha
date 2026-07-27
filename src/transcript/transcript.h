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

// A point-in-time copy of a Transcript for readers that must not hold its lock while they work.
// The revision and history epoch let a renderer decide what changed since the frame it drew last.
struct TranscriptSnapshot {
    std::vector<TranscriptEntry> entries;
    std::size_t revision{};
    std::optional<EntryId> open_entry_id;
    std::size_t history_epoch{};
};

class Transcript;

// Borrowed read access to a Transcript for callers that need its entries without copying them,
// such as preparing a completion request. The view holds the transcript lock for its whole
// lifetime, so the spans and references it hands out stay valid only while it lives; keep it
// short and never block the writer behind it.
class TranscriptReadView {
public:
    ~TranscriptReadView() = default;

    TranscriptReadView(const TranscriptReadView&) = delete;
    TranscriptReadView& operator=(const TranscriptReadView&) = delete;
    TranscriptReadView(TranscriptReadView&&) = delete;
    TranscriptReadView& operator=(TranscriptReadView&&) = delete;

    std::span<const TranscriptEntry> entries() const noexcept;
    std::size_t revision() const noexcept;
    std::optional<EntryId> open_entry_id() const noexcept;
    OffrecordSpan offrecord_span() const noexcept;
    std::size_t history_epoch() const noexcept;

private:
    friend class Transcript;

    explicit TranscriptReadView(const Transcript& transcript);

    std::unique_lock<std::mutex> lock_;
    const std::vector<TranscriptEntry>* entries_{};
    std::optional<EntryId> open_entry_id_;
    OffrecordSpan offrecord_;
    std::size_t history_epoch_{};
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

// The live transcript of one session, and the only mutable transcript state shared between the
// UI and the agent runtime. It serializes appends, streaming updates, and history resets
// behind a mutex, allows at most one open streaming entry, and offers readers either a snapshot
// copy or a locked read view. It depends on nothing beyond the entry model declared above.
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
    void open_silent_offrecord();
    void extend_silent_offrecord();
    void restore_silent_offrecord();

    TranscriptSnapshot snapshot() const;
    std::vector<TranscriptEntry> entries() const;
    std::optional<EntryId> open_entry_id() const;
    TranscriptReadView read() const;

private:
    friend class TranscriptReadView;
    void require_next_id(EntryId entry_id) const;
    EntryId boundary() const;

    mutable std::mutex mutex_;
    std::vector<TranscriptEntry> entries_;
    std::size_t revision_{};
    std::optional<EntryId> open_entry_id_;
    OffrecordSpan offrecord_;
    std::size_t history_epoch_{};
};

} // namespace cha
