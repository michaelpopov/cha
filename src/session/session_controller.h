#pragma once

#include "agents/agent.h"
#include "agents/agent_registry.h"
#include "agents/persona.h"
#include "session/generation_status.h"
#include "session/forum_characters.h"
#include "session/session_database.h"
#include "session/session_lease.h"
#include "session/session_state.h"
#include "session/session_change.h"
#include "transcript/transcript.h"
#include "util/wake_notifier.h"
#include "util/thread_pool.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

// One live chat session, and the only object a front end needs in order to run a chat. It has two
// halves: read-only session state (transcript, forum characters, default agent, generation status) and
// commands (submit a prompt, clear, stop, switch the default agent, drain agent events),
// each returning a SessionChange instead of touching a frontend. It owns the Transcript,
// SessionJournal, AgentRegistry, and the state of the in-flight response batch. Command syntax,
// mentions, and transport formats belong to front ends, not here.
class SessionController {
public:
    // Fault-injection seam used to fail foreground activation after selection
    // but before durable state changes.
    using ActivationHook = std::function<void(std::size_t)>;

    [[nodiscard]] static std::unique_ptr<SessionController> from_shared_definitions(
        std::vector<AgentDefinition> definitions,
        SharedPersonaRoster personas,
        ParticipantId initial_default_agent_id,
        std::filesystem::path database_path,
        SessionLease lease,
        WakeNotifier& notifier,
        SessionRestore restored = {});
    // Test-only counterpart for controller tests that intentionally do not
    // claim a fixture database's production lease.
    [[nodiscard]] static std::unique_ptr<SessionController> from_definitions_for_testing(
        std::vector<AgentDefinition> definitions,
        PersonaRoster personas,
        ParticipantId initial_default_agent_id,
        std::filesystem::path database_path,
        WakeNotifier& notifier,
        SessionRestore restored = {});
    // Test-only construction and activation fault injection. These seams live
    // here because the otherwise private controller owns both dependencies.
    [[nodiscard]] static std::unique_ptr<SessionController> from_backends_for_testing(
        std::vector<std::unique_ptr<CompletionBackend>> backends,
        PersonaRoster personas,
        ParticipantId initial_default_agent_id,
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
    // Builds an owning state value on this controller's owner thread for
    // asynchronous presentation consumers.
    [[nodiscard]] SessionState state() const;
    // Owner-thread-only proof of a text-only extension since a previously
    // published core cursor. Any ambiguity deliberately returns no append.
    [[nodiscard]] std::optional<SessionAppendProjection> text_append_since(
        const SessionStateCursor& cursor) const;
    const ForumCharacters& characters() const { return characters_; }
    const ParticipantId& default_agent_id() const { return default_agent_id_; }

    // --- Session commands (mutate, then report semantic changes) --------------
    [[nodiscard]] SessionChange submit_prompt(
        std::string_view author_id,
        std::string text,
        std::string handle = {});
    [[nodiscard]] SessionChange clear_transcript();
    [[nodiscard]] SessionChange open_offrecord();
    [[nodiscard]] SessionChange extend_offrecord();
    [[nodiscard]] SessionChange restore_offrecord();
    // The web text grammar submits handles; resolution and all target
    // validation stay here with the forum's authoritative character set.
    [[nodiscard]] SessionChange start_multicast(
        std::string_view author_id,
        std::string text,
        std::vector<std::string> handles);
    [[nodiscard]] SessionChange session_information();
    [[nodiscard]] SessionChange agent_information();
    [[nodiscard]] SessionChange set_default_agent(std::string_view handle);
    [[nodiscard]] SessionChange set_default_agent_by_id(std::string_view id);
    [[nodiscard]] SessionChange request_stop();
    [[nodiscard]] SessionChange handle_agent_event(AgentEvent event);
    [[nodiscard]] SessionEventBatch receive_events(std::size_t max_events);
    void shutdown();

private:
    struct GenerationView {
        bool active{};
        std::optional<RequestId> request_id;
        std::string_view agent_id;
        std::string_view agent_name;
        ResponsePhase phase{ResponsePhase::waiting};
        std::string_view reasoning_text;
    };

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
        SharedPersonaRoster personas,
        ParticipantId initial_default_agent_id,
        std::filesystem::path database_path,
        SessionLease lease,
        WakeNotifier& notifier,
        SessionRestore restored);
    SessionController(
        std::vector<std::unique_ptr<CompletionBackend>> backends,
        PersonaRoster personas,
        ParticipantId initial_default_agent_id,
        std::filesystem::path database_path,
        WakeNotifier& notifier,
        SessionRestore restored,
        ActivationHook before_activation);

    void initialize(SessionRestore restored);
    [[nodiscard]] GenerationView generation_view() const noexcept;
    [[nodiscard]] GenerationStatus snapshot_generation() const;
    bool busy() const noexcept;
    SessionChange busy_notice() const;
    [[nodiscard]] std::optional<EntryIdentity> resolve_author(
        std::string_view author_id,
        SessionChange& change) const;
    void start_batch(
        EntryIdentity author,
        std::string text,
        std::vector<CharacterInfo> targets,
        SharedCompletionHistory history,
        SessionChange& change);
    [[nodiscard]] SessionChange start_resolved_multicast(
        std::string_view author_id,
        std::string text,
        std::vector<CharacterInfo> targets);
    void activate_current_run(SessionChange& change);
    void start_next_batch_run(SessionChange& change);
    void finish_batch_run(SessionChange& change);
    void finish_batch(SessionChange& change);
    void finish_aborted_batch(SessionChange& change);
    void poll_abort_cleanup(SessionChange& change);
    void append_batch_notice(const SessionChange& change);
    void abandon_batch();
    void apply(const AgentDelta& event, SessionChange& change);
    void apply(const AgentCompleted& event, SessionChange& change);
    void apply(const AgentCancelled& event, SessionChange& change);
    void apply(const AgentFailed& event, SessionChange& change);
    void fail_active_response(
        std::string message,
        ParticipantId participant_id,
        SessionChange& change);
    void finish_response_entry(EntryStatus status);
    TranscriptEntry response_entry(EntryStatus status) const;
    bool matches(RequestId request_id) const;

    // lease_ is declared before journal_ so reverse destruction keeps the lock
    // through journal destruction and explicit controller shutdown.
    SessionLease lease_;
    Transcript transcript_;
    SessionJournal journal_;
    // Explicit shutdown joins this pool while registry_ is still alive.
    // One worker per backend intentionally admits full-width multicast work.
    // Declaration order is the fallback only for construction failures.
    ThreadPool worker_pool_;
    AgentRegistry registry_;
    ForumCharacters characters_;
    SharedPersonaRoster personas_;
    ParticipantId default_agent_id_;
    RequestId next_request_id_{1};
    EntryId next_entry_id_{1};
    std::optional<ActiveResponse> active_;
    std::optional<ResponseBatch> batch_;
    ActivationHook before_activation_;
    bool shutdown_{};
};

} // namespace cha
