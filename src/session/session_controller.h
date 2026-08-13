#pragma once

#include "agents/character.h"
#include "agents/generation_batch.h"
#include "agents/generation_executor.h"
#include "chat/persona.h"
#include "session/controller_update.h"
#include "session/controller_view.h"
#include "session/generation_status.h"
#include "session/forum_characters.h"
#include "session/session_database.h"
#include "session/session_lease.h"
#include "chat/transcript.h"
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
// halves: read-only session state (transcript, forum characters, defaults, generation
// status) and commands (submit a prompt, clear, stop, switch defaults, drain
// generation events),
// each returning a ControllerUpdate instead of touching a frontend. It owns the Transcript,
// SessionJournal, GenerationExecutor, and the one in-flight GenerationBatch. Command syntax,
// mentions, and transport formats belong to front ends, not here.
class SessionController {
public:
    // Fault-injection seam used to fail foreground activation after selection
    // but before durable state changes.
    using ActivationHook = std::function<void(std::size_t)>;

    [[nodiscard]] static std::unique_ptr<SessionController> from_shared_definitions(
        std::vector<CharacterDefinition> definitions,
        SharedPersonaRoster personas,
        CharacterId initial_default_character_id,
        std::string initial_default_persona_id,
        std::filesystem::path database_path,
        SessionLease lease,
        WakeNotifier& notifier,
        SessionRestore restored = {});
    // Test-only counterpart for controller tests that intentionally do not
    // claim a fixture database's production lease.
    [[nodiscard]] static std::unique_ptr<SessionController> from_definitions_for_testing(
        std::vector<CharacterDefinition> definitions,
        PersonaRoster personas,
        CharacterId initial_default_character_id,
        std::filesystem::path database_path,
        WakeNotifier& notifier,
        SessionRestore restored = {});
    // Test-only construction and activation fault injection. These seams live
    // here because the otherwise private controller owns both dependencies.
    [[nodiscard]] static std::unique_ptr<SessionController> from_backends_for_testing(
        std::vector<std::unique_ptr<ModelBackend>> backends,
        PersonaRoster personas,
        CharacterId initial_default_character_id,
        std::filesystem::path database_path,
        WakeNotifier& notifier,
        SessionRestore restored = {},
        ActivationHook before_activation = {});
    ~SessionController();
    SessionController(const SessionController&) = delete;
    SessionController& operator=(const SessionController&) = delete;

    // --- Session state (read-only) --------------------------------------------
    [[nodiscard]] bool is_generating() const noexcept;
    // A borrowed read model for full projection. It is valid only on this
    // controller's owner thread and only until the next mutation, so callers
    // must consume it synchronously.
    [[nodiscard]] ControllerView view() const noexcept;

    // --- Session commands (mutate, then report semantic changes) --------------
    [[nodiscard]] ControllerUpdate submit_prompt(
        std::string_view author_id,
        std::string text,
        std::string handle = {});
    [[nodiscard]] ControllerUpdate clear_transcript();
    [[nodiscard]] ControllerUpdate open_offrecord();
    [[nodiscard]] ControllerUpdate extend_offrecord();
    [[nodiscard]] ControllerUpdate restore_offrecord();
    // The web text grammar submits handles; resolution and all target
    // validation stay here with the forum's authoritative character set.
    [[nodiscard]] ControllerUpdate start_multicast(
        std::string_view author_id,
        std::string text,
        std::vector<std::string> handles);
    [[nodiscard]] ControllerUpdate session_information();
    [[nodiscard]] ControllerUpdate character_information();
    [[nodiscard]] ControllerUpdate set_default_character(std::string_view handle);
    [[nodiscard]] ControllerUpdate set_default_character_by_id(std::string_view id);
    [[nodiscard]] ControllerUpdate set_default_persona(std::string_view handle);
    [[nodiscard]] ControllerUpdate request_stop();
    void rename(std::string_view label);
    [[nodiscard]] ControllerUpdate handle_generation_event(GenerationEvent event);
    [[nodiscard]] ControllerEventBatch receive_events(std::size_t max_events);
    void shutdown();

private:
    struct ActiveResponse {
        RequestId request_id{};
        EntryId response_entry_id{};
        CharacterId character_id;
        std::string character_display_name;
        ResponsePhase phase{ResponsePhase::waiting};
        std::string reasoning_text;
    };

    SessionController(
        std::vector<CharacterDefinition> definitions,
        SharedPersonaRoster personas,
        CharacterId initial_default_character_id,
        std::string initial_default_persona_id,
        std::filesystem::path database_path,
        SessionLease lease,
        WakeNotifier& notifier,
        SessionRestore restored);
    SessionController(
        std::vector<std::unique_ptr<ModelBackend>> backends,
        PersonaRoster personas,
        CharacterId initial_default_character_id,
        std::string initial_default_persona_id,
        std::filesystem::path database_path,
        WakeNotifier& notifier,
        SessionRestore restored,
        ActivationHook before_activation);

    void initialize(SessionRestore restored, std::string_view initial_persona_id);
    [[nodiscard]] ControllerGenerationView generation_view() const noexcept;
    bool busy() const noexcept;
    ControllerUpdate busy_notice() const;
    [[nodiscard]] std::optional<EntryIdentity> resolve_author(
        std::string_view author_id,
        ControllerUpdate& update) const;
    void start_batch(
        EntryIdentity author,
        std::string text,
        std::vector<CharacterMetadata> targets,
        SharedModelHistory history,
        ControllerUpdate& update);
    [[nodiscard]] ControllerUpdate start_resolved_multicast(
        std::string_view author_id,
        std::string text,
        std::vector<CharacterMetadata> targets);
    void activate_current_run(ControllerUpdate& update);
    void start_next_batch_run(ControllerUpdate& update);
    void finish_batch_run(ControllerUpdate& update);
    void finish_batch(ControllerUpdate& update);
    void finish_aborted_batch(ControllerUpdate& update);
    void poll_abort_cleanup(ControllerUpdate& update);
    void append_batch_notice(const ControllerUpdate& update);
    // The one release path: waits until no execution can reach a backend,
    // destroys the batch, and clears the controller's notice accumulation so it
    // cannot leak into a later operation.
    void release_batch() noexcept;
    void apply(const GenerationEventDelta& event, ControllerUpdate& update);
    void apply(const GenerationCompleted& event, ControllerUpdate& update);
    void apply(const GenerationCancelled& event, ControllerUpdate& update);
    void apply(const GenerationFailed& event, ControllerUpdate& update);
    void fail_active_response(
        std::string message,
        ParticipantId participant_id,
        ControllerUpdate& update);
    void finish_response_entry(EntryStatus status);
    TranscriptEntry response_entry(EntryStatus status) const;
    bool matches(RequestId request_id) const;

    // lease_ is declared before journal_ so reverse destruction keeps the lock
    // through journal destruction and explicit controller shutdown.
    SessionLease lease_;
    Transcript transcript_;
    SessionJournal journal_;
    // Explicit shutdown joins this pool while generation_executor_ is still
    // alive. One worker per backend intentionally admits full-width multicast
    // work. Declaration order — pool, then executor, then batch — is the
    // fallback only for construction failures.
    ThreadPool worker_pool_;
    GenerationExecutor generation_executor_;
    ForumCharacters characters_;
    SharedPersonaRoster personas_;
    CharacterId default_character_id_;
    // Borrowed from personas_, which is immutable and outlives the controller,
    // so the view can hand out the persona's own strings.
    const Persona* default_persona_{};
    RequestId next_request_id_{1};
    EntryId next_entry_id_{1};
    std::optional<ActiveResponse> active_;
    std::optional<GenerationBatch> batch_;
    // Presentation state only: the batch owns runs, foreground selection,
    // cancellation, and generation finalization.
    std::string terminal_notices_;
    bool stop_notice_recorded_{};
    ActivationHook before_activation_;
    bool shutdown_{};
};

} // namespace cha
