#pragma once

#include "agents/agent_definition.h"
#include "agents/agent_registry.h"
#include "application/generation_status.h"
#include "application/turn_engine.h"
#include "conversation/conversation.h"
#include "storage/session_database.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

struct CoordinatorUpdate {
    bool render_needed{};
    bool end_session{};
    bool clear_input{};
    std::optional<std::string> notice;
};

// UI-facing owner of one live chat session.
//
// Has two faces on purpose:
// 1. Session state — read-only accessors over conversation, agents, and generation.
// 2. Session commands — operations that mutate the session and return CoordinatorUpdate
//    side effects for the UI (render / clear input / notice / end session).
class ChatCoordinator {
public:
    [[nodiscard]] static std::unique_ptr<ChatCoordinator> from_definitions(
        std::vector<AgentDefinition> definitions,
        std::filesystem::path database_path,
        ConversationRestore restored = {});
    [[nodiscard]] static std::unique_ptr<ChatCoordinator> from_backends_for_testing(
        std::vector<std::unique_ptr<CompletionBackend>> backends,
        std::filesystem::path database_path,
        ConversationRestore restored = {});

    ~ChatCoordinator();
    ChatCoordinator(const ChatCoordinator&) = delete;
    ChatCoordinator& operator=(const ChatCoordinator&) = delete;

    // --- Session state (read model) -------------------------------------------
    const Conversation& conversation() const { return conversation_; }
    GenerationStatus generation_status() const { return turns_.generation_status(); }
    const AgentRoster& roster() const { return registry_.roster(); }
    const ParticipantId& default_agent_id() const { return default_agent_id_; }
    int notification_fd() const { return registry_.notification_fd(); }

    // --- Session commands (write model → UI side effects) ---------------------
    // Return value carries render/end/clear/notice side effects the UI must apply.
    [[nodiscard]] CoordinatorUpdate submit_prompt(
        std::string text,
        std::string handle = {});
    [[nodiscard]] CoordinatorUpdate clear_conversation();
    [[nodiscard]] CoordinatorUpdate session_information();
    [[nodiscard]] CoordinatorUpdate agent_information();
    [[nodiscard]] CoordinatorUpdate set_default_agent(std::string_view handle);
    [[nodiscard]] CoordinatorUpdate request_stop();
    [[nodiscard]] CoordinatorUpdate handle_agent_event(AgentEvent event);
    [[nodiscard]] CoordinatorUpdate receive();
    void shutdown();

private:
    ChatCoordinator(
        std::vector<AgentDefinition> definitions,
        std::filesystem::path database_path,
        ConversationRestore restored);
    ChatCoordinator(
        std::vector<std::unique_ptr<CompletionBackend>> backends,
        std::filesystem::path database_path,
        ConversationRestore restored);

    void initialize(ConversationRestore restored);
    CoordinatorUpdate busy_notice() const;
    static void merge_turn(CoordinatorUpdate& update, TurnUpdate turn);

    Conversation conversation_;
    ConversationJournal journal_;
    AgentRegistry registry_;
    TurnEngine turns_;
    ParticipantId default_agent_id_;
    bool shutdown_{};
};

} // namespace cha
