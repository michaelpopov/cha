#pragma once

#include "conversation.h"
#include "request_id.h"

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace cha {

// Identifies and materializes a turn that lacked a terminal record after recovery.
struct InterruptedTurn {
    RequestId request_id{};
    ConversationEntry error_entry;
};

// Contains the replayed typed transcript and the next durable identifiers.
struct ConversationRestore {
    std::vector<ConversationEntry> entries;
    RequestId next_request_id{1};
    EntryId next_entry_id{1};
    std::vector<InterruptedTurn> interrupted_turns;
};

// Initializes a missing journal and removes an incomplete final record left by a crash.
void prepare_conversation_file(const std::filesystem::path& path);

[[nodiscard]] ConversationRestore load_conversation_state(const std::filesystem::path& path);
[[nodiscard]] std::vector<ConversationEntry> load_conversation_file(const std::filesystem::path& path);
void save_conversation_file(const std::filesystem::path& path, const Conversation& conversation);

// Durably appends typed turn lifecycle and standalone transcript events to one JSONL session.
class ConversationJournal {
public:
    explicit ConversationJournal(std::filesystem::path path);

    ConversationJournal(const ConversationJournal&) = delete;
    ConversationJournal& operator=(const ConversationJournal&) = delete;

    void append(const ConversationEntry& entry);
    void start_turn(
        RequestId request_id,
        std::string_view agent_id,
        const ConversationEntry& prompt);
    void complete_turn(RequestId request_id, const ConversationEntry& response);
    void cancel_turn(RequestId request_id, const ConversationEntry& response);
    void fail_turn(RequestId request_id, const ConversationEntry& error);
    void clear();

private:
    std::filesystem::path path_;
    std::mutex mutex_;
};

} // namespace cha
