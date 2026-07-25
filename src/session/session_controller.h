#pragma once

#include "agents/agent.h"
#include "agents/agent_registry.h"
#include "session/generation_status.h"
#include "session/response_controller.h"
#include "transcript/transcript.h"
#include "session/session_database.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

// The side effects one controller command leaves for the front end to apply: redraw, clear the
// input, show a notice, end the session. Returning them keeps every UI type out of the controller.
struct SessionUpdate {
    bool render_needed{};
    bool end_session{};
    bool clear_input{};
    std::optional<std::string> notice;
};

// One live chat session, and the only object a front end needs in order to run a chat. It has two
// halves: read-only session state (transcript, roster, default agent, generation status) and
// commands (submit a prompt, clear, stop, switch the default agent, drain agent events),
// each returning a SessionUpdate instead of touching the UI. It composes Transcript,
// SessionJournal, AgentRegistry, and ResponseController, and allows only one turn at a time.
// Command syntax, mentions, and transport formats belong to front ends, not here.
class SessionController {
public:
    [[nodiscard]] static std::unique_ptr<SessionController> from_definitions(
        std::vector<AgentDefinition> definitions,
        std::filesystem::path database_path,
        SessionRestore restored = {});
    [[nodiscard]] static std::unique_ptr<SessionController> from_backends_for_testing(
        std::vector<std::unique_ptr<CompletionBackend>> backends,
        std::filesystem::path database_path,
        SessionRestore restored = {});

    ~SessionController();
    SessionController(const SessionController&) = delete;
    SessionController& operator=(const SessionController&) = delete;

    // --- Session state (read-only) --------------------------------------------
    const Transcript& transcript() const { return transcript_; }
    GenerationStatus generation_status() const { return response_.generation_status(); }
    const AgentRoster& roster() const { return registry_.roster(); }
    const ParticipantId& default_agent_id() const { return default_agent_id_; }
    int notification_fd() const { return registry_.notification_fd(); }

    // --- Session commands (mutate, then report UI side effects) ---------------
    // Return value carries render/end/clear/notice side effects the UI must apply.
    [[nodiscard]] SessionUpdate submit_prompt(
        std::string text,
        std::string handle = {});
    [[nodiscard]] SessionUpdate clear_transcript();
    [[nodiscard]] SessionUpdate session_information();
    [[nodiscard]] SessionUpdate agent_information();
    [[nodiscard]] SessionUpdate set_default_agent(std::string_view handle);
    [[nodiscard]] SessionUpdate request_stop();
    [[nodiscard]] SessionUpdate handle_agent_event(AgentEvent event);
    [[nodiscard]] SessionUpdate receive();
    void shutdown();

private:
    SessionController(
        std::vector<AgentDefinition> definitions,
        std::filesystem::path database_path,
        SessionRestore restored);
    SessionController(
        std::vector<std::unique_ptr<CompletionBackend>> backends,
        std::filesystem::path database_path,
        SessionRestore restored);

    void initialize(SessionRestore restored);
    SessionUpdate busy_notice() const;
    static void merge_response(SessionUpdate& update, ResponseUpdate response);

    Transcript transcript_;
    SessionJournal journal_;
    AgentRegistry registry_;
    ResponseController response_;
    ParticipantId default_agent_id_;
    bool shutdown_{};
};

} // namespace cha
