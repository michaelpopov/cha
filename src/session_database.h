#pragma once

#include "conversation.h"
#include "request_id.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

// Describes the identity and display metadata stored inside one session database.
struct SessionDatabaseMetadata {
    std::string id;
    std::string room;
    std::string persona;
    std::string label;
};

// Describes a pending repair for a turn that lacked a terminal state.
// error_entry is not durable until ConversationJournal::fail_turn succeeds.
struct InterruptedTurn {
    RequestId request_id{};
    ConversationEntry error_entry;
};

// Contains the persisted transcript and identifiers reserved past pending repairs.
// A caller opening a live session must finalize every interrupted_turn before
// making any other journal write.
struct ConversationRestore {
    std::vector<ConversationEntry> entries;
    RequestId next_request_id{1};
    EntryId next_entry_id{1};
    std::vector<InterruptedTurn> interrupted_turns;
};

// Initializes a temporary sibling and atomically publishes one session database.
// Returns false without replacing anything if the destination path exists.
[[nodiscard]] bool create_session_database(
    const std::filesystem::path& path,
    const SessionDatabaseMetadata& metadata);

[[nodiscard]] SessionDatabaseMetadata read_session_database_metadata(
    const std::filesystem::path& path);
[[nodiscard]] ConversationRestore load_conversation_state(
    const std::filesystem::path& path);
[[nodiscard]] std::vector<ConversationEntry> load_conversation_entries(
    const std::filesystem::path& path);

// Persists typed transcript and turn-lifecycle changes as SQLite transactions.
class ConversationJournal {
public:
    explicit ConversationJournal(std::filesystem::path path);
    ~ConversationJournal();

    ConversationJournal(const ConversationJournal&) = delete;
    ConversationJournal& operator=(const ConversationJournal&) = delete;

    void append(const ConversationEntry& entry);
    void start_turn(
        RequestId request_id,
        std::string_view agent_id,
        const ConversationEntry& prompt);
    void complete_turn(RequestId request_id, const ConversationEntry& response);
    void cancel_turn(RequestId request_id, std::optional<ConversationEntry> response);
    void fail_turn(RequestId request_id, const ConversationEntry& error);
    void clear();

private:
    class Impl;

    std::unique_ptr<Impl> impl_;
};

} // namespace cha
