#pragma once

#include "agent_definition.h"
#include "agent_registry.h"
#include "conversation.h"
#include "generation_status.h"
#include "session_database.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace cha {

struct CoordinatorUpdate {
    bool render_needed{};
    bool end_session{};
    bool clear_input{};
    std::optional<std::string> notice;
};

class ChatCoordinator {
public:
    ChatCoordinator(
        std::vector<AgentDefinition> definitions,
        std::filesystem::path database_path,
        ConversationRestore restored = {});
    ChatCoordinator(
        std::vector<std::unique_ptr<CompletionBackend>> backends,
        std::filesystem::path database_path,
        ConversationRestore restored = {});
    ~ChatCoordinator();
    ChatCoordinator(const ChatCoordinator&) = delete;
    ChatCoordinator& operator=(const ChatCoordinator&) = delete;

    [[nodiscard]] const Conversation& conversation() const;
    [[nodiscard]] bool generating() const;
    [[nodiscard]] GenerationStatus generation_status() const;
    [[nodiscard]] bool show_addressing() const;
    [[nodiscard]] const AgentRoster& roster() const;
    [[nodiscard]] int notification_fd() const;
    [[nodiscard]] CoordinatorUpdate handle_input(std::string input);
    [[nodiscard]] CoordinatorUpdate request_stop();
    [[nodiscard]] CoordinatorUpdate handle_agent_event(AgentEvent event);
    [[nodiscard]] CoordinatorUpdate receive();
    void shutdown();

private:
    struct ActiveTurn {
        RequestId request_id{};
        EntryId response_entry_id{};
        ParticipantId agent_id;
        std::string agent_name;
        bool entry_open{};
    };
    void initialize(ConversationRestore restored);
    [[nodiscard]] CoordinatorUpdate submit(std::string input);
    void clear();
    [[nodiscard]] std::string handle_notice(
        std::string_view handle,
        const HandleResolution& resolution) const;
    [[nodiscard]] std::string roster_notice() const;
    void apply(const AgentDelta&, CoordinatorUpdate&);
    void apply(const AgentCompleted&, CoordinatorUpdate&);
    void apply(const AgentCancelled&, CoordinatorUpdate&);
    void apply(const AgentFailed&, CoordinatorUpdate&);
    void fail_active_turn(std::string message, ParticipantId participant_id, CoordinatorUpdate&);
    void finish_response_entry(CompletionStatus status);
    [[nodiscard]] ConversationEntry response_entry(CompletionStatus status) const;
    [[nodiscard]] bool matches(RequestId request_id) const;

    Conversation conversation_;
    ConversationJournal journal_;
    AgentRegistry registry_;
    ParticipantId default_agent_id_;
    RequestId next_request_id_{1};
    EntryId next_entry_id_{1};
    std::optional<ActiveTurn> active_;
    bool show_addressing_{};
    bool shutdown_{};
};

} // namespace cha
