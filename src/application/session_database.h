#pragma once

#include "conversation/conversation.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

// The identity a session database carries inside itself, so a file can be checked against the
// session and room it claims to belong to before anything is read from it.
struct SessionDatabaseMetadata {
    std::string id;
    std::string room;
    std::string label;
};

// A turn found without a terminal state on restore, left behind when a run ended mid-generation.
// error_entry is the record that closes it, and becomes durable only once fail_turn() succeeds.
struct InterruptedTurn {
    RequestId request_id{};
    ConversationEntry error_entry;
};

// Everything needed to resume a stored session: the durable transcript, the ID counters to carry
// on from, and any turns left unfinished. A caller opening a live session must finalize every
// interrupted turn before it makes any other journal write.
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

SessionDatabaseMetadata read_session_database_metadata(
    const std::filesystem::path& path);
ConversationRestore load_conversation_state(
    const std::filesystem::path& path);
std::vector<ConversationEntry> load_conversation_entries(
    const std::filesystem::path& path);

// The durable half of one session. It writes transcript entries and turn transitions — start,
// complete, cancel, fail — to a single SQLite file as transactions, so a run that dies mid-turn
// can be restored and repaired afterwards. It accepts only storable ConversationEntry values:
// streaming state and reasoning text never reach disk.
class ConversationJournal {
public:
    explicit ConversationJournal(std::filesystem::path path);
    ~ConversationJournal();

    ConversationJournal(const ConversationJournal&) = delete;
    ConversationJournal& operator=(const ConversationJournal&) = delete;

    void append(const ConversationEntry& entry);
    void start_turn(
        RequestId request_id,
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
