#pragma once

#include "conversation.h"
#include "request_id.h"

#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace cha {

// Identifies a turn that was durably accepted but lacked a terminal record after recovery.
struct InterruptedTurn {
    RequestId request_id{};
};

// Contains the replayed transcript, request sequence, and any turns interrupted by a crash.
struct ConversationRestore {
    std::vector<ConversationMessage> messages;
    RequestId next_request_id{1};
    std::vector<InterruptedTurn> interrupted_turns;
};

// Initializes a missing journal and removes an incomplete final record left by a crash.
void prepare_conversation_file(const std::filesystem::path& path);

[[nodiscard]] ConversationRestore load_conversation_state(const std::filesystem::path& path);
[[nodiscard]] std::vector<ConversationMessage> load_conversation_file(const std::filesystem::path& path);
void save_conversation_file(const std::filesystem::path& path, const Conversation& conversation);

// Durably appends identified turn lifecycle and standalone transcript events to one JSONL session.
class ConversationJournal {
public:
    explicit ConversationJournal(std::filesystem::path path);

    ConversationJournal(const ConversationJournal&) = delete;
    ConversationJournal& operator=(const ConversationJournal&) = delete;

    void append(const ConversationMessage& message);
    void start_turn(RequestId request_id, std::string_view agent_id, std::string_view prompt);
    void complete_turn(
        RequestId request_id,
        std::string_view author,
        std::string_view response);
    void cancel_turn(
        RequestId request_id,
        std::string_view author,
        std::string_view partial_response);
    void fail_turn(RequestId request_id, std::string_view error);
    void clear();

private:
    std::filesystem::path path_;
    std::mutex mutex_;
};

} // namespace cha
