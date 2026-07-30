#pragma once

#include "agents/agent.h"
#include "agents/agent_registry.h"
#include "session/generation_status.h"
#include "session/forum_personas.h"
#include "session/session_database.h"
#include "transcript/transcript.h"
#include "util/wake_notifier.h"
#include "util/thread_pool.h"

#include <filesystem>
#include <functional>
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
    // nullopt leaves the frontend's current notice unchanged, an empty string
    // clears it, and a non-empty string replaces it.
    std::optional<std::string> notice;
};

// One live chat session, and the only object a front end needs in order to run a chat. It has two
// halves: read-only session state (transcript, forum personas, default agent, generation status) and
// commands (submit a prompt, clear, stop, switch the default agent, drain agent events),
// each returning a SessionUpdate instead of touching the UI. It owns the Transcript,
// SessionJournal, AgentRegistry, and the state of the in-flight response batch. Command syntax,
// mentions, and transport formats belong to front ends, not here.
class SessionController {
public:
    // Fault-injection seam used to fail foreground activation after selection
    // but before durable state changes.
    using ActivationHook = std::function<void(std::size_t)>;

    [[nodiscard]] static std::unique_ptr<SessionController> from_definitions(
        std::vector<AgentDefinition> definitions,
        std::filesystem::path database_path,
        WakeNotifier& notifier,
        SessionRestore restored = {});
    // Test-only construction and activation fault injection. These seams live
    // here because the otherwise private controller owns both dependencies.
    [[nodiscard]] static std::unique_ptr<SessionController> from_backends_for_testing(
        std::vector<std::unique_ptr<CompletionBackend>> backends,
        std::filesystem::path database_path,
        WakeNotifier& notifier,
        SessionRestore restored = {},
        ActivationHook before_activation = {});

    ~SessionController();
    SessionController(const SessionController&) = delete;
    SessionController& operator=(const SessionController&) = delete;

    // --- Session state (read-only) --------------------------------------------
    const Transcript& transcript() const { return transcript_; }
    [[nodiscard]] bool is_generating() const noexcept;
    GenerationStatus generation_status() const;
    const ForumPersonas& personas() const { return personas_; }
    const ParticipantId& default_agent_id() const { return default_agent_id_; }

    // --- Session commands (mutate, then report UI side effects) ---------------
    // Return value carries render/end/clear/notice side effects the UI must apply.
    [[nodiscard]] SessionUpdate submit_prompt(
        std::string text,
        std::string handle = {});
    [[nodiscard]] SessionUpdate clear_transcript();
    [[nodiscard]] SessionUpdate open_offrecord();
    [[nodiscard]] SessionUpdate extend_offrecord();
    [[nodiscard]] SessionUpdate restore_offrecord();
    [[nodiscard]] SessionUpdate start_multicast(
        std::string text,
        std::vector<PersonaInfo> targets);
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

    struct ResponseBatch {
        SharedCompletionHistory history;
        std::vector<RunSpec> runs;
        std::size_t foreground_index{};
        bool abort_requested{};
        bool stop_notice_recorded{};
        std::string terminal_notices;
    };

    SessionController(
        std::vector<AgentDefinition> definitions,
        std::filesystem::path database_path,
        WakeNotifier& notifier,
        SessionRestore restored);
    SessionController(
        std::vector<std::unique_ptr<CompletionBackend>> backends,
        std::filesystem::path database_path,
        WakeNotifier& notifier,
        SessionRestore restored,
        ActivationHook before_activation);

    void initialize(SessionRestore restored);
    bool busy() const noexcept;
    SessionUpdate busy_notice() const;
    void start_batch(
        std::string text,
        std::vector<PersonaInfo> targets,
        SharedCompletionHistory history,
        SessionUpdate& update);
    void activate_current_run(SessionUpdate& update);
    void start_next_batch_run(SessionUpdate& update);
    void finish_batch_run(SessionUpdate& update);
    void finish_batch(SessionUpdate& update);
    void finish_aborted_batch(SessionUpdate& update);
    void poll_abort_cleanup(SessionUpdate& update);
    void append_batch_notice(const SessionUpdate& update);
    void abandon_batch();
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
    // Explicit shutdown joins this pool while registry_ is still alive.
    // One worker per backend intentionally admits full-width multicast work.
    // Declaration order is the fallback only for construction failures.
    ThreadPool worker_pool_;
    AgentRegistry registry_;
    ForumPersonas personas_;
    ParticipantId default_agent_id_;
    RequestId next_request_id_{1};
    EntryId next_entry_id_{1};
    std::optional<ActiveResponse> active_;
    std::optional<ResponseBatch> batch_;
    ActivationHook before_activation_;
    bool shutdown_{};
};

} // namespace cha
