#pragma once

#include "agents/agent.h"
#include "agents/agent_registry.h"
#include "session/generation_status.h"
#include "transcript/transcript.h"
#include "session/session_database.h"

#include <optional>
#include <string>
#include <string_view>

namespace cha {

// What the caller must do after a ResponseController call: redraw when the transcript changed,
// and show the notice when one is set.
struct ResponseUpdate {
    bool render_needed{};
    std::optional<std::string> notice;
};

// Owns the single in-flight response, keeping turn mechanics out of SessionController. It starts a
// turn by making the prompt durable and submitting it, then applies each AgentEvent to the live
// Transcript and the SessionJournal until exactly one terminal state is reached. It drives
// AgentRegistry for submission and cancellation, holds the entry and request ID counters restored
// with a session, and reports the current GenerationStatus.
class ResponseController {
public:
    ResponseController(
        Transcript& transcript,
        SessionJournal& journal,
        AgentRegistry& registry);

    void restore(SessionRestore restored);

    GenerationStatus generation_status() const;
    RequestId next_request_id() const { return next_request_id_; }
    EntryId next_entry_id() const { return next_entry_id_; }

    // Starts a response for the already-resolved target agent. On success, notice is set to "".
    ResponseUpdate start(
        std::string text,
        const AgentInfo& target);

    ResponseUpdate apply(AgentEvent event);

private:
    struct ActiveResponse {
        RequestId request_id{};
        EntryId response_entry_id{};
        ParticipantId agent_id;
        std::string agent_name;
        ResponsePhase phase{ResponsePhase::waiting};
        std::string reasoning_text;
    };

    void apply(const AgentDelta& event, ResponseUpdate& update);
    void apply(const AgentCompleted& event, ResponseUpdate& update);
    void apply(const AgentCancelled& event, ResponseUpdate& update);
    void apply(const AgentFailed& event, ResponseUpdate& update);
    void fail_active_response(
        std::string message,
        ParticipantId participant_id,
        ResponseUpdate& update);
    void finish_response_entry(EntryStatus status);
    TranscriptEntry response_entry(EntryStatus status) const;
    bool matches(RequestId request_id) const;

    Transcript& transcript_;
    SessionJournal& journal_;
    AgentRegistry& registry_;
    RequestId next_request_id_{1};
    EntryId next_entry_id_{1};
    std::optional<ActiveResponse> active_;
};

} // namespace cha
