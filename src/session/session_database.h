#pragma once

#include "chat/session_identity.h"
#include "chat/transcript.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

using SessionKey = std::int64_t;

struct SessionDatabaseMetadata {
    std::string id;
    std::string forum;
    std::string label;
};

struct InterruptedTurn {
    RequestId request_id{};
    TranscriptEntry error_entry;
};

struct SessionRestore {
    std::vector<TranscriptEntry> entries;
    RequestId next_request_id{1};
    EntryId next_entry_id{1};
    std::vector<InterruptedTurn> interrupted_turns;
};

struct LoadedSessionDatabase {
    SessionKey session_key{};
    SessionDatabaseMetadata metadata;
    SessionRestore restore;
};

// Creates a workspace-schema database containing exactly one forum and one
// session. Production uses this for Welcome; tests use it as a compact real
// journal fixture. The inserted session_key is always 1.
[[nodiscard]] bool create_session_database(
    const std::filesystem::path& path,
    const SessionDatabaseMetadata& metadata);

SessionDatabaseMetadata read_session_database_metadata(
    const std::filesystem::path& path);

SessionRestore load_session_state(const std::filesystem::path& path);

LoadedSessionDatabase load_session_database(
    const std::filesystem::path& path,
    const FullSessionId& expected_identity);

// Current-epoch durable rows only. Unlike restore, this read does not
// synthesize interruption errors.
std::vector<TranscriptEntry> load_session_history(
    const std::filesystem::path& path,
    const FullSessionId& expected_identity);

// One live actor owns one journal connection. Different journals may point at
// the same workspace database, but every statement is scoped by session_key.
class SessionJournal {
public:
    SessionJournal(std::filesystem::path path, SessionKey session_key);
    // Convenience for a database created by create_session_database().
    explicit SessionJournal(std::filesystem::path path);
    ~SessionJournal();

    SessionJournal(const SessionJournal&) = delete;
    SessionJournal& operator=(const SessionJournal&) = delete;

    void start_turn(RequestId request_id, const TranscriptEntry& prompt);
    void record_entry(const TranscriptEntry& entry);
    void complete_turn(RequestId request_id, const TranscriptEntry& response);
    void cancel_turn(RequestId request_id, std::optional<TranscriptEntry> response);
    void fail_turn(RequestId request_id, const TranscriptEntry& error);
    void clear();
    void rename(std::string_view label);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace cha
