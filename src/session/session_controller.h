#pragma once

#include "characters/character.h"
#include "providers/providers.h"
#include "chat/persona.h"
#include "session/controller_update.h"
#include "session/controller_view.h"
#include "session/generation_status.h"
#include "session/session_database.h"
#include "chat/session_identity.h"
#include "chat/transcript.h"
#include "util/wake_notifier.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

class Workspace;
struct WorkspaceForum;

// One live chat session, and the only object a front end needs in order to run a chat. It has two
// halves: read-only session state (transcript, forum characters, defaults, generation
// status) and commands (submit a prompt, stop, switch defaults, drain
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
        SessionKey session_key,
        Providers& providers,
        std::shared_ptr<WakeNotifier> notifier,
        SessionRestore restored,
        FullSessionId identity);

    // Tests use the same Workspace data path, but may own an injected provider
    // executor and use an activation fault hook.
    [[nodiscard]] static std::unique_ptr<SessionController> from_workspace_for_testing(
        CharacterId initial_default_character_id,
        std::string initial_default_persona_id,
        std::filesystem::path database_path,
        std::shared_ptr<Providers> providers,
        std::shared_ptr<WakeNotifier> notifier,
        SessionRestore restored = {},
        ActivationHook before_activation = {},
        FullSessionId identity = {},
        SessionKey session_key = 1);
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
    [[nodiscard]] ControllerUpdate cover_conversation();
    [[nodiscard]] ControllerUpdate uncover_conversation();
    // The web text grammar submits handles; resolution and all target
    // validation stay here with the forum's authoritative character set.
    [[nodiscard]] ControllerUpdate start_multicast(
        std::string_view author_id,
        std::string text,
        std::vector<std::string> handles);
    [[nodiscard]] ControllerUpdate set_default_character_by_id(std::string_view id);
    [[nodiscard]] ControllerUpdate request_stop();
    void rename(std::string_view label);
    [[nodiscard]] ControllerUpdate handle_generation_event(GenerationEvent event);
    [[nodiscard]] ControllerEventBatch receive_events(std::size_t max_events);
    void shutdown();

private:
    enum class AnswerTimestampState {
        checking,
        skipping_whitespace,
        passthrough,
    };

    struct ActiveResponse {
        RequestId request_id{};
        EntryId response_entry_id{};
        CharacterId character_id;
        std::string character_display_name;
        ResponsePhase phase{ResponsePhase::waiting};
        std::string reasoning_text;
        AnswerTimestampState answer_timestamp_state{
            AnswerTimestampState::checking};
        std::string pending_answer_text;
        std::string pending_source_reference;
        // The stamp of the live streaming entry, captured when it opens so the
        // record later handed to the journal carries the same created_at.
        std::int64_t response_created_at{};
    };

    SessionController(
        CharacterId initial_default_character_id,
        std::string initial_default_persona_id,
        std::filesystem::path database_path,
        SessionKey session_key,
        Providers& providers,
        std::shared_ptr<WakeNotifier> notifier,
        SessionRestore restored,
        ActivationHook before_activation = {},
        FullSessionId identity = {},
        std::shared_ptr<Providers> providers_owner = {});

    void initialize(SessionRestore restored, std::string_view initial_persona_id);
    [[nodiscard]] SharedCharacterDefinition definition_for(
        std::string_view id) const;
    [[nodiscard]] std::shared_ptr<const Workspace> workspace() const;
    [[nodiscard]] SharedPersonaRoster current_personas() const;
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
    void append_answer_text(std::string text, ControllerUpdate& update);
    [[nodiscard]] std::string filter_answer_timestamp(std::string_view text);
    [[nodiscard]] std::string filter_source_references(std::string_view text);
    void flush_pending_answer_text(ControllerUpdate& update);
    void fail_active_response(
        std::string message,
        ParticipantId participant_id,
        ControllerUpdate& update);
    void finish_response_entry(EntryStatus status);
    TranscriptEntry response_entry(EntryStatus status) const;
    bool matches(RequestId request_id) const;

    Transcript transcript_;
    SessionJournal journal_;
    // Production borrows its process-owned executor. Test-backed controllers
    // own their injected executor because no process composition root exists.
    std::shared_ptr<Providers> providers_owner_;
    Providers& providers_;
    // The shared notifier is copied into each request so late worker wakes
    // never borrow this session.
    std::shared_ptr<WakeNotifier> notifier_;
    FullSessionId identity_;
    CharacterId default_character_id_;
    std::string default_persona_id_;
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
