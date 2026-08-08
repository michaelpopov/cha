#pragma once

#include "session/session_identity.h"
#include "chat/transcript.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

// The identity a session database carries inside itself, so a file can be checked against the
// session and forum it claims to belong to before anything is read from it.
struct SessionDatabaseMetadata {
    std::string id;
    std::string forum;
    std::string label;
};

// A turn found without a terminal state on restore, left behind when a run ended mid-generation.
// error_entry is the record that closes it, and becomes durable only once fail_turn() succeeds.
struct InterruptedTurn {
    RequestId request_id{};
    TranscriptEntry error_entry;
};

// Everything needed to resume a stored session: the durable transcript, the ID counters to carry
// on from, and any turns left unfinished. A caller opening a live session must finalize every
// interrupted turn before it makes any other journal write.
struct SessionRestore {
    std::vector<TranscriptEntry> entries;
    RequestId next_request_id{1};
    EntryId next_entry_id{1};
    std::vector<InterruptedTurn> interrupted_turns;
};

// Everything one leased session database has to say about itself: who it
// claims to be, and the state to resume it from. An owning value; the
// connection it was read through is already closed.
struct LoadedSessionDatabase {
    SessionDatabaseMetadata metadata;
    SessionRestore restore;
};

// Initializes a temporary sibling and atomically publishes one session database.
// Returns false without replacing anything if the destination path exists.
[[nodiscard]] bool create_session_database(
    const std::filesystem::path& path,
    const SessionDatabaseMetadata& metadata);

// The identity check every reader of a stored database owes it: what the file
// says it is, against what the caller opened it as. Throws on a mismatch.
void validate_session_database_identity(
    const std::filesystem::path& path,
    const SessionIdentity& expected_identity,
    const SessionDatabaseMetadata& metadata);

// Just enough to list a session, in one lightweight read.
SessionDatabaseMetadata read_session_database_metadata(
    const std::filesystem::path& path);
SessionRestore load_session_state(
    const std::filesystem::path& path);
// The authoritative read for opening: metadata and restore state through one
// connection, with the identity checked before the transcript is touched. The
// caller must already hold the session's lease.
LoadedSessionDatabase load_session_database(
    const std::filesystem::path& path,
    const SessionIdentity& expected_identity);

// The durable half of one session. It writes turn transitions — start, complete,
// cancel, fail — to a single SQLite file as transactions, so a run that dies
// mid-turn can be restored and repaired afterwards. It accepts only terminal
// TranscriptEntry values; active streaming state never reaches disk.
class SessionJournal {
public:
    explicit SessionJournal(std::filesystem::path path);
    ~SessionJournal();

    SessionJournal(const SessionJournal&) = delete;
    SessionJournal& operator=(const SessionJournal&) = delete;

    void start_turn(
        RequestId request_id,
        const TranscriptEntry& prompt);
    void complete_turn(RequestId request_id, const TranscriptEntry& response);
    void cancel_turn(RequestId request_id, std::optional<TranscriptEntry> response);
    void fail_turn(RequestId request_id, const TranscriptEntry& error);
    void clear();

private:
    class Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace cha
