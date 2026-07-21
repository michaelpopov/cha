#pragma once

#include "agent_definition.h"
#include "agent_info.h"
#include "agent_protocol.h"
#include "agent_worker.h"
#include "conversation.h"
#include "session_database.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace cha {

// Summarizes coordinator work that may require rendering or ending the UI loop.
struct CoordinatorUpdate {
    bool render_needed{};
    bool end_session{};
    bool clear_input{};
    std::optional<std::string> notice;
};

// Owns one chat session's worker, transcript, journal, commands, and turn lifecycle.
class ChatCoordinator {
public:
    ChatCoordinator(
        AgentDefinition definition,
        std::filesystem::path database_path,
        ConversationRestore restored = {});
    // Accepts an alternate completion backend for coordinator and application tests.
    ChatCoordinator(
        std::unique_ptr<CompletionBackend> backend,
        std::filesystem::path database_path,
        ConversationRestore restored = {});
    ~ChatCoordinator();

    ChatCoordinator(const ChatCoordinator&) = delete;
    ChatCoordinator& operator=(const ChatCoordinator&) = delete;

    [[nodiscard]] const Conversation& conversation() const;
    [[nodiscard]] bool generating() const;
    [[nodiscard]] int notification_fd() const;

    [[nodiscard]] CoordinatorUpdate handle_input(std::string input);
    [[nodiscard]] CoordinatorUpdate request_stop();
    // Applies one typed worker event independently of channel polling.
    [[nodiscard]] CoordinatorUpdate handle_agent_event(AgentEvent event);
    [[nodiscard]] CoordinatorUpdate receive();
    void shutdown();

private:
    // Tracks the exact response entry associated with the only active single-agent turn.
    struct ActiveTurn {
        RequestId request_id{};
        EntryId response_entry_id{};
        std::string response;
        bool entry_open{};
    };

    void initialize(ConversationRestore restored);
    [[nodiscard]] CoordinatorUpdate submit(std::string prompt);
    void clear();
    void add_notice(std::string text);
    void apply(const AgentDelta& event, CoordinatorUpdate& update);
    void apply(const AgentCompleted& event, CoordinatorUpdate& update);
    void apply(const AgentCancelled& event, CoordinatorUpdate& update);
    void apply(const AgentFailed& event, CoordinatorUpdate& update);
    void fail_active_turn(
        std::string message,
        ParticipantId participant_id,
        CoordinatorUpdate& update);
    void finish_response_entry(CompletionStatus status);
    [[nodiscard]] ConversationEntry response_entry(CompletionStatus status) const;
    [[nodiscard]] bool matches(RequestId request_id) const;

    Conversation conversation_;
    ConversationJournal journal_;
    AgentWorker worker_;
    AgentInfo agent_info_;
    RequestId next_request_id_{1};
    EntryId next_entry_id_{1};
    std::optional<ActiveTurn> active_;
    bool shutdown_{};
};

} // namespace cha
