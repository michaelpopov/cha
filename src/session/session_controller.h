#pragma once

#include "agents/agent.h"
#include "agents/agent_registry.h"
#include "session/generation_status.h"
#include "session/session_database.h"
#include "transcript/transcript.h"

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
// each returning a SessionUpdate instead of touching the UI. It owns the Transcript,
// SessionJournal, AgentRegistry, and the state of the single in-flight turn. Command syntax,
// mentions, and transport formats belong to front ends, not here.
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
    GenerationStatus generation_status() const;
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
    struct ActiveResponse {
        RequestId request_id{};
        EntryId response_entry_id{};
        ParticipantId agent_id;
        std::string agent_name;
        ResponsePhase phase{ResponsePhase::waiting};
        std::string reasoning_text;
    };

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
    SessionUpdate start_response(std::string text, const AgentInfo& target);
    void apply(const AgentDelta& event, SessionUpdate& update);
    void apply(const AgentCompleted& event, SessionUpdate& update);
    void apply(const AgentCancelled& event, SessionUpdate& update);
    void apply(const AgentFailed& event, SessionUpdate& update);
    void fail_active_response(
        std::string message,
        ParticipantId participant_id,
        SessionUpdate& update);
    void finish_response_entry(EntryStatus status);
    TranscriptEntry response_entry(EntryStatus status) const;
    bool matches(RequestId request_id) const;

    Transcript transcript_;
    SessionJournal journal_;
    AgentRegistry registry_;
    ParticipantId default_agent_id_;
    RequestId next_request_id_{1};
    EntryId next_entry_id_{1};
    std::optional<ActiveResponse> active_;
    bool shutdown_{};
};

} // namespace cha
