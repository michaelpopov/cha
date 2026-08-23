#pragma once

#include "characters/character.h"
#include "providers/providers.h"
#include "chat/persona.h"
#include "session/controller_update.h"
#include "session/controller_view.h"
#include "session/generation_status.h"
#include "session/session_database.h"
#include "session/session_lease.h"
#include "session/session_identity.h"
#include "chat/transcript.h"
#include "util/wake_notifier.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cha {

class Workspace;
struct WorkspaceForum;

// One live chat session, and the only object a front end needs in order to run a chat. It has two
// halves: read-only session state (transcript, forum characters, defaults, generation
// status) and commands (submit a prompt, clear, stop, switch defaults, drain
// generation events), each returning a ControllerUpdate instead of touching a frontend. It owns
// the Transcript, SessionJournal, and session-visible request handles. Provider execution itself
// belongs to the process-owned Providers instance. Command syntax,
// mentions, and transport formats belong to front ends, not here.
class SessionController {
public:
    // Fault-injection seam used to fail foreground activation after selection
    // but before durable state changes.
    using ActivationHook = std::function<void(std::size_t)>;

    [[nodiscard]] static std::unique_ptr<SessionController> from_workspace(
        CharacterId initial_default_character_id,
        std::string initial_default_persona_id,
        std::filesystem::path database_path,
        SessionLease lease,
        Providers& providers,
        std::shared_ptr<WakeNotifier> notifier,
        SessionRestore restored,
        SessionIdentity identity);

    // Tests use the same Workspace data path, but may own an injected provider
    // executor and use an inactive lease or an activation fault hook.
    [[nodiscard]] static std::unique_ptr<SessionController> from_workspace_for_testing(
        CharacterId initial_default_character_id,
        std::string initial_default_persona_id,
        std::filesystem::path database_path,
        SessionLease lease,
        std::shared_ptr<Providers> providers,
        std::shared_ptr<WakeNotifier> notifier,
        SessionRestore restored = {},
        ActivationHook before_activation = {},
        SessionIdentity identity = {});
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
    // Runtime appearance override for the current default character: an empty
    // name reports the override state, "default" restores the configured style,
    // anything else is resolved and swapped in. Session-scoped only; nothing is
    // persisted. Mutating forms carry a browser-visible snapshot.
    [[nodiscard]] ControllerUpdate set_session_style(std::string_view name);
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
        // The stamp of the live streaming entry, captured when it opens so the
        // record later handed to the journal carries the same created_at.
        std::int64_t response_created_at{};
    };

    SessionController(
        CharacterId initial_default_character_id,
        std::string initial_default_persona_id,
        std::filesystem::path database_path,
        SessionLease lease,
        Providers& providers,
        std::shared_ptr<WakeNotifier> notifier,
        SessionRestore restored,
        ActivationHook before_activation = {},
        SessionIdentity identity = {},
        std::shared_ptr<Providers> providers_owner = {});

    void initialize(SessionRestore restored, std::string_view initial_persona_id);
    [[nodiscard]] SharedCharacterDefinition definition_for(
        std::string_view id) const;
    [[nodiscard]] std::shared_ptr<const Workspace> workspace() const;
    [[nodiscard]] SharedPersonaRoster current_personas() const;
    [[nodiscard]] std::vector<CharacterRuntimeInfo> current_runtime_info(
        const Workspace& workspace,
        const WorkspaceForum& forum) const;
    [[nodiscard]] CharacterMetadata styled_character(
        const Workspace& workspace,
        const CharacterMetadata& character) const;
    [[nodiscard]] CharacterAppearance resolve_style(
        const Workspace& workspace,
        std::string_view name) const;
    [[nodiscard]] ControllerGenerationView generation_view() const noexcept;
    ControllerUpdate busy_notice() const;
    [[nodiscard]] std::optional<EntryIdentity> resolve_author(
        std::string_view author_id,
        ControllerUpdate& update) const;
    // Records one human message addressed to the reserved null target `-`:
    // persisted and shown like any other message, but no model is called and
    // no reply is produced.
    void record_monologue(
        std::string_view author_id,
        std::string text,
        ControllerUpdate& update);
    void start_generation(
        EntryIdentity author,
        std::string text,
        std::vector<CharacterMetadata> targets,
        SharedModelHistory history,
        ControllerUpdate& update);
    [[nodiscard]] ControllerUpdate start_resolved_multicast(
        std::string_view author_id,
        std::string text,
        std::vector<CharacterMetadata> targets);
    void activate_run(const RunSpec& run, std::size_t foreground_index,
                      ControllerUpdate& update);
    void finish_generation_run(ControllerUpdate& update);
    void cancel_generation_requests() noexcept;
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
    // Production borrows its process-owned executor. Test-backed controllers
    // own their injected executor because no process composition root exists.
    std::shared_ptr<Providers> providers_owner_;
    Providers& providers_;
    // The shared notifier is copied into each request so late worker wakes
    // never borrow this session.
    std::shared_ptr<WakeNotifier> notifier_;
    SessionIdentity identity_;
    CharacterId default_character_id_;
    std::string default_persona_id_;
    // Selected names are session-scoped and never persisted.
    std::unordered_map<CharacterId, std::string> style_overrides_;
    RequestId next_request_id_{1};
    EntryId next_entry_id_{1};
    std::optional<ActiveResponse> active_;
    struct ActiveGeneration {
        std::vector<std::shared_ptr<ProviderRequest>> requests;
        std::size_t foreground_index{};
        bool cancellation_requested{};
        std::string terminal_notices;
    };
    std::optional<ActiveGeneration> generation_;
    ActivationHook before_activation_;
    bool shutdown_{};
};

} // namespace cha
