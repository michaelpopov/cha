#pragma once

#include "agents/agent.h"
#include "agents/agent_registry.h"
#include "application/generation_status.h"
#include "application/response_controller.h"
#include "conversation/conversation.h"
#include "application/session_database.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

// The side effects one coordinator command leaves for the interface to apply: redraw, clear the
// input, show a notice, end the session. Returning them keeps every UI type out of the coordinator.
struct CoordinatorUpdate {
    bool render_needed{};
    bool end_session{};
    bool clear_input{};
    std::optional<std::string> notice;
};

// One live chat session, and the only object an interface needs in order to run a chat. It has two
// faces: read-only session state (conversation, roster, default agent, generation status) and
// structured commands (submit a prompt, clear, stop, switch the default agent, drain agent events),
// each returning a CoordinatorUpdate instead of touching the UI. It composes Conversation,
// ConversationJournal, AgentRegistry, and ResponseController, and allows only one turn at a time.
// Command syntax, mentions, and transport formats belong to interfaces, not here.
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
    GenerationStatus generation_status() const { return response_.generation_status(); }
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
    static void merge_response(CoordinatorUpdate& update, ResponseUpdate response);

    Conversation conversation_;
    ConversationJournal journal_;
    AgentRegistry registry_;
    ResponseController response_;
    ParticipantId default_agent_id_;
    bool shutdown_{};
};

} // namespace cha
