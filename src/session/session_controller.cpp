#include "session/session_controller.h"

#include "session/session_label.h"
#include "util/crypto.h"
#include "util/logging.h"
#include "util/text.h"
#include "workspace/workspace.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <exception>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <variant>

namespace cha {
namespace {

constexpr std::string_view generation_stopped_notice = "Generation stopped";

enum class TimestampPrefixResult {
    incomplete,
    matched,
    rejected,
};

struct TimestampPrefixMatch {
    TimestampPrefixResult result;
    std::size_t end{};
};

TimestampPrefixMatch match_timestamp_prefix(std::string_view text) {
    std::size_t position = 0;
    while (position < text.size() && is_space(text[position])) {
        ++position;
    }
    if (position == text.size()) {
        return {TimestampPrefixResult::incomplete};
    }

    constexpr std::string_view shape = "[####-##-##T##:##:##";
    for (const char expected : shape) {
        if (position == text.size()) {
            return {TimestampPrefixResult::incomplete};
        }
        const char actual = text[position++];
        const bool matches = expected == '#'
            ? actual >= '0' && actual <= '9'
            : actual == expected;
        if (!matches) {
            return {TimestampPrefixResult::rejected};
        }
    }

    if (position == text.size()) {
        return {TimestampPrefixResult::incomplete};
    }
    if (text[position] == '.') {
        ++position;
        const std::size_t fraction_start = position;
        while (position < text.size()
               && text[position] >= '0' && text[position] <= '9') {
            ++position;
        }
        if (position == text.size()) {
            return {TimestampPrefixResult::incomplete};
        }
        if (position == fraction_start) {
            return {TimestampPrefixResult::rejected};
        }
    }
    if (text[position++] != 'Z') {
        return {TimestampPrefixResult::rejected};
    }
    if (position == text.size()) {
        return {TimestampPrefixResult::incomplete};
    }
    if (text[position++] != ']') {
        return {TimestampPrefixResult::rejected};
    }
    return {TimestampPrefixResult::matched, position};
}

void append_line(std::string& text, std::string_view line) {
    if (!text.empty()) text += '\n';
    text += line;
}

std::int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void log_persistence_failure(
    const std::string& action,
    std::string_view details) noexcept {
    try {
        log_error(
            "Session persistence failed: " + action + "; reason="
            + std::string(details));
    } catch (...) {
        log_error("Session persistence failed");
    }
}

template<typename Operation>
void persist(std::string action, Operation&& operation) {
    try {
        operation();
    } catch (const std::exception& error) {
        log_persistence_failure(action, error.what());
        throw std::runtime_error(
            "Failed to " + std::move(action) + ": " + error.what());
    }
}

std::string request_action(
    std::string_view action,
    RequestId request_id,
    std::string_view character_display_name) {
    return std::string(action) + " request " + std::to_string(request_id)
        + " for @" + std::string(character_display_name);
}

std::string prompt_cache_key(
    const FullSessionId& identity,
    std::string_view character_id) {
    if (identity.forum_id.empty() || identity.session_id.empty() || character_id.empty()) {
        return {};
    }
    const std::string key = identity.forum_id + "/" + identity.session_id
        + "/" + std::string(character_id);
    const bool ascii = std::ranges::all_of(key, [](unsigned char character) {
        return character <= 0x7f;
    });
    if (ascii && key.size() <= 64) return key;

    const Sha256Digest digest = sha256_digest(key);
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2);
    for (const unsigned char byte : digest) {
        result.push_back(hex[byte >> 4]);
        result.push_back(hex[byte & 0x0f]);
    }
    return result;
}

std::string format_handle_resolution_notice(
    std::string_view handle,
    const HandleResolution& resolution,
    const Workspace& workspace,
    std::string_view forum_id) {
    if (resolution.match == HandleMatch::unknown) {
        return "Unknown character @" + std::string(handle)
            + ". Characters in this forum: "
            + workspace.forum_handle_list(forum_id);
    }
    std::string result =
        "Ambiguous character @" + std::string(handle) + ": matches ";
    for (std::size_t index{}; index < resolution.candidates.size(); ++index) {
        if (index) result += ", ";
        result += "@" + resolution.candidates[index]->display_name;
    }
    return result + ". Type more of the name.";
}

std::string format_duplicate_character_notice(std::string_view display_name) {
    return "Multicast target @" + std::string(display_name) + " is duplicated";
}

} // namespace

std::unique_ptr<SessionController> SessionController::from_workspace(
    CharacterId initial_default_character_id,
    std::string initial_default_persona_id,
    std::filesystem::path database_path,
    SessionKey session_key,
    Providers& providers,
    std::shared_ptr<WakeNotifier> notifier,
    SessionRestore restored,
    FullSessionId identity) {
    return std::unique_ptr<SessionController>(new SessionController(
        std::move(initial_default_character_id),
        std::move(initial_default_persona_id), std::move(database_path),
        session_key, providers,
        std::move(notifier), std::move(restored), {}, std::move(identity)));
}

std::unique_ptr<SessionController> SessionController::from_workspace_for_testing(
    ParticipantId initial_default_character_id,
    std::string initial_default_persona_id,
    std::filesystem::path database_path,
    std::shared_ptr<Providers> providers,
    std::shared_ptr<WakeNotifier> notifier,
    SessionRestore restored,
    ActivationHook before_activation,
    FullSessionId identity,
    SessionKey session_key) {
    if (!providers) throw std::invalid_argument("Session controller requires providers");
    Providers& provider = *providers;
    return std::unique_ptr<SessionController>(new SessionController(
        std::move(initial_default_character_id),
        std::move(initial_default_persona_id),
        std::move(database_path), session_key, provider,
        std::move(notifier),
        std::move(restored), std::move(before_activation), std::move(identity),
        std::move(providers)));
}

SessionController::SessionController(
    ParticipantId initial_default_character_id,
    std::string initial_default_persona_id,
    std::filesystem::path path,
    SessionKey session_key,
    Providers& providers,
    std::shared_ptr<WakeNotifier> notifier,
    SessionRestore restored,
    ActivationHook before_activation,
    FullSessionId identity,
    std::shared_ptr<Providers> providers_owner)
    : journal_(std::move(path), session_key),
      providers_owner_(std::move(providers_owner)),
      providers_(providers),
      notifier_(std::move(notifier)),
      identity_(std::move(identity)),
      default_character_id_(std::move(initial_default_character_id)),
      before_activation_(std::move(before_activation)) {
    if (!notifier_) throw std::invalid_argument("Session controller requires a wake notifier");
    initialize(std::move(restored), initial_default_persona_id);
}

SessionController::~SessionController() {
    try {
        shutdown();
    } catch (...) {
    }
}

void SessionController::initialize(
    SessionRestore restored,
    std::string_view initial_persona_id) {
    const std::shared_ptr<const Workspace> current = workspace();
    if (!current->find_forum_character(
            identity_.forum_id, default_character_id_)) {
        throw std::invalid_argument(
            "Initial default character ID is not in the forum roster");
    }
    const SharedPersonaRoster personas = current_personas();
    if (initial_persona_id.empty()) {
        default_persona_id_ = personas->front().id;
    } else {
        const auto found =
            std::ranges::find(*personas, initial_persona_id, &Persona::id);
        if (found == personas->end()) {
            throw std::invalid_argument(
                "Initial default persona ID is not in the persona roster");
        }
        default_persona_id_ = found->id;
    }
    transcript_.replace_entries(std::move(restored.entries));
    next_request_id_ = restored.next_request_id;
    next_entry_id_ = restored.next_entry_id;
    for (const InterruptedTurn& turn : restored.interrupted_turns) {
        persist(
            request_action(
                "persist interrupted-turn repair for",
                turn.request_id,
                turn.error_entry.participant_id),
            [this, &turn] {
                journal_.fail_turn(turn.request_id, turn.error_entry);
            });
        transcript_.add_entry(turn.error_entry);
    }
}

std::shared_ptr<const Workspace> SessionController::workspace() const {
    std::shared_ptr<const Workspace> result = getws();
    if (!result) throw std::runtime_error("Workspace is not loaded");
    if (result->find_forum(identity_.forum_id) == nullptr) {
        throw std::runtime_error(
            "Session forum '" + identity_.forum_id
            + "' is absent from the current workspace");
    }
    return result;
}

SharedPersonaRoster SessionController::current_personas() const {
    const std::shared_ptr<const Workspace> current = workspace();
    return std::make_shared<const PersonaRoster>(
        current->personas().begin(), current->personas().end());
}

SharedCharacterDefinition SessionController::definition_for(
    std::string_view id) const {
    const std::shared_ptr<const Workspace> current = workspace();
    const WorkspaceForumMember* const member =
        current->find_forum_member(identity_.forum_id, id);
    if (member == nullptr) return {};
    return std::make_shared<const CharacterDefinition>(
        current->character_definition(identity_.forum_id, id));
}

ControllerGenerationView SessionController::generation_view() const noexcept {
    if (generation_ && generation_->cancellation_requested && active_) {
        const RunSpec& run = generation_->requests[generation_->foreground_index]->run();
        return {
            .active = true,
            .request_id = run.request_id,
            .character_id = run.target.id,
            .character_display_name = run.target.display_name,
            .phase = ResponsePhase::stopping,
        };
    }
    return {
        .active = is_generating(),
        .request_id = active_ ? std::optional<RequestId>(active_->request_id)
                              : std::nullopt,
        .character_id = active_ ? std::string_view(active_->character_id)
                            : std::string_view{},
        .character_display_name = active_ ? std::string_view(active_->character_display_name)
                              : std::string_view{},
        .phase = active_ ? active_->phase : ResponsePhase::waiting,
        .reasoning_text = active_ ? std::string_view(active_->reasoning_text)
                                  : std::string_view{},
    };
}

ControllerView SessionController::view() const noexcept {
    // Every field borrows live controller storage. The caller consumes it
    // before the next mutation on this thread; nothing here allocates.
    return {
        .default_character_id = default_character_id_,
        .default_persona_id = default_persona_id_,
        .transcript = transcript_.view(),
        .generation = generation_view(),
    };
}

bool SessionController::is_generating() const noexcept {
    return generation_.has_value();
}

ControllerUpdate SessionController::busy_notice() const {
    return {.notice = std::string(generation_in_progress_notice)};
}

std::optional<EntryIdentity> SessionController::resolve_author(
    std::string_view author_id,
    ControllerUpdate& update) const {
    const SharedPersonaRoster personas = current_personas();
    const auto author = std::find_if(
        personas->begin(), personas->end(),
        [author_id](const Persona& persona) { return persona.id == author_id; });
    if (author == personas->end()) {
        update.notice = "Unknown persona ID '" + std::string(author_id) + "'";
        return std::nullopt;
    }
    return EntryIdentity{author->id, author->display_name};
}

void SessionController::record_monologue(
    std::string_view author_id,
    std::string text,
    ControllerUpdate& update) {
    if (text.empty()) {
        update.notice = "Message to @- is empty";
        return;
    }
    std::optional<EntryIdentity> author = resolve_author(author_id, update);
    if (!author) return;
    TranscriptEntry entry = make_human_entry({
        .id = next_entry_id_++,
        .author = std::move(*author),
        .addressed_to = {std::string(null_agent_handle),
                         std::string(null_agent_name)},
        .text = std::move(text),
    });
    persist(
        "record a message addressed to @-",
        [this, &entry] { journal_.record_entry(entry); });
    transcript_.add_entry(entry);
    update.input_consumed = true;
    update.notice = "";
    require_snapshot(update);
}

ControllerUpdate SessionController::submit_prompt(
    std::string_view author_id,
    std::string text,
    std::string handle) {
    if (shutdown_) {
        return {.notice = "Request could not be dispatched"};
    }
    if (is_generating()) {
        return busy_notice();
    }
    if (text.empty() && handle.empty()) {
        return {};
    }

    ControllerUpdate update;
    const std::shared_ptr<const Workspace> current = workspace();
    const CharacterMetadata* target = nullptr;
    if (handle.empty()) {
        if (default_character_id_ == null_agent_handle) {
            record_monologue(author_id, std::move(text), update);
            return update;
        }
        target = current->find_forum_character(
            identity_.forum_id, default_character_id_);
    } else {
        if (handle == null_agent_handle) {
            record_monologue(author_id, std::move(text), update);
            return update;
        }
        const HandleResolution resolution =
            current->resolve_forum_handle(identity_.forum_id, handle);
        if (resolution.match != HandleMatch::resolved) {
            update.notice = format_handle_resolution_notice(
                handle, resolution, *current, identity_.forum_id);
            return update;
        }
        target = resolution.character;
    }
    if (!target) {
        throw std::logic_error("Default character is not among the forum characters");
    }
    if (text.empty()) {
        update.notice = "Prompt for @" + target->display_name + " is empty";
        return update;
    }

    std::optional<EntryIdentity> author = resolve_author(author_id, update);
    if (!author) return update;

    SharedModelHistory history =
        std::make_shared<const ModelHistory>(
            transcript_.model_history());
    update.input_consumed = true;
    start_generation(
        std::move(*author),
        std::move(text),
        std::vector<CharacterMetadata>{*target},
        std::move(history),
        update);
    return update;
}

void SessionController::start_generation(
    EntryIdentity author,
    std::string text,
    std::vector<CharacterMetadata> targets,
    SharedModelHistory history,
    ControllerUpdate& update) {
    if (!history || targets.empty()) {
        throw std::invalid_argument(
            "Generation requires history and at least one target");
    }
    std::vector<ProviderRequestInput> inputs;
    inputs.reserve(targets.size());
    for (CharacterMetadata& target : targets) {
        SharedCharacterDefinition definition = definition_for(target.id);
        if (!definition) {
            throw std::logic_error("Generation target has no character definition");
        }
        const std::string cache_key = prompt_cache_key(identity_, target.id);
        inputs.push_back({
            .character = std::move(definition),
            .generation = {
                .history = history,
                .run = {
                    .session = identity_,
                    .request_id = next_request_id_++,
                    .target = std::move(target),
                    .author = author,
                    .prompt_text = text,
                    .prompt_cache_key = cache_key,
                    .created_at = unix_now(),
                },
            },
        });
    }

    // Durable state exists before immediate request start. A returned request
    // always has a terminal event, including closed admission and launch
    // failure, so post-commit operational failures cannot strand this turn.
    generation_.emplace();
    try {
        generation_->requests.reserve(inputs.size());
        activate_run(inputs.front().generation.run, 0, update);
    } catch (...) {
        generation_.reset();
        throw;
    }
    for (ProviderRequestInput& input : inputs) {
        generation_->requests.push_back(
            providers_.make_request(std::move(input), notifier_));
    }
}

void SessionController::activate_run(
    const RunSpec& run,
    std::size_t foreground_index,
    ControllerUpdate& update) {
    TranscriptEntry prompt = make_human_entry({
        .id = next_entry_id_++,
        .author = run.author,
        .addressed_to = {run.target.id, run.target.display_name},
        .text = run.prompt_text,
        .request_id = run.request_id,
        .created_at = run.created_at,
    });
    ActiveResponse response{
        .request_id = run.request_id,
        .response_entry_id = next_entry_id_++,
        .character_id = run.target.id,
        .character_display_name = run.target.display_name,
        .phase = ResponsePhase::waiting,
    };

    if (before_activation_) {
        before_activation_(foreground_index);
    }
    persist(
        request_action(
            "persist start of",
            run.request_id,
            run.target.display_name),
        [this, &run, &prompt] {
            journal_.start_turn(run.request_id, prompt);
        });
    try {
        transcript_.add_entry(prompt);
    } catch (...) {
        TranscriptEntry error = make_error_entry(
            next_entry_id_++,
            "Failed to add the submitted prompt to the transcript",
            run.request_id,
            run.target.id);
        persist(
            request_action(
                "persist failure of",
                run.request_id,
                run.target.display_name),
            [this, &run, &error] {
                journal_.fail_turn(run.request_id, error);
            });
        throw;
    }
    active_ = std::move(response);
    require_snapshot(update);
    update.notice = "";
}

void SessionController::finish_generation_run(ControllerUpdate& update) {
    if (!generation_) return;
    if (generation_->cancellation_requested
        && (!update.notice || *update.notice != generation_stopped_notice)) {
        if (!update.notice) update.notice.emplace();
        append_line(*update.notice, generation_stopped_notice);
    }
    if (update.notice && !update.notice->empty()) {
        append_line(generation_->terminal_notices, *update.notice);
    }
    const std::size_t foreground = generation_->foreground_index;
    generation_->requests[foreground].reset();
    if (shutdown_ || generation_->cancellation_requested
        || foreground + 1 == generation_->requests.size()) {
        const std::string terminal_notices = generation_->terminal_notices;
        generation_.reset();
        require_snapshot(update);
        if (!terminal_notices.empty()) update.notice = terminal_notices;
        return;
    }
    ++generation_->foreground_index;
    const RunSpec& run = generation_->requests[generation_->foreground_index]->run();
    try {
        activate_run(run, generation_->foreground_index, update);
    } catch (...) {
        cancel_generation_requests();
        generation_.reset();
        throw;
    }
}

void SessionController::cancel_generation_requests() noexcept {
    for (const std::shared_ptr<ProviderRequest>& request : generation_->requests) {
        if (request) request->cancel();
    }
}

ControllerUpdate SessionController::cover_conversation(
    std::optional<EntryId> through_entry_id) {
    if (is_generating()) {
        return busy_notice();
    }
    if (!transcript_.cover(next_entry_id_, through_entry_id)) {
        return {
            .notice = "Cover target was not found",
        };
    }
    ++next_entry_id_;
    return {
        .state = SnapshotRequired{},
        .input_consumed = true,
    };
}

ControllerUpdate SessionController::uncover_conversation() {
    if (is_generating()) {
        return busy_notice();
    }
    if (!transcript_.uncover(next_entry_id_)) {
        return {
            .input_consumed = true,
            .notice = "Nothing to uncover",
        };
    }
    ++next_entry_id_;
    return {
        .state = SnapshotRequired{},
        .input_consumed = true,
    };
}

ControllerUpdate SessionController::delete_turn(EntryId response_entry_id) {
    if (is_generating()) {
        return busy_notice();
    }
    if (!transcript_.can_delete_turn(response_entry_id)) {
        return {.notice = "Response was not found"};
    }
    persist(
        "delete a transcript turn",
        [this, response_entry_id] { journal_.delete_turn(response_entry_id); });
    if (!transcript_.delete_turn(response_entry_id)) {
        throw std::logic_error("Durable turn was missing from the transcript");
    }
    return {
        .state = SnapshotRequired{},
        .input_consumed = true,
    };
}

ControllerUpdate SessionController::start_multicast(
    std::string_view author_id,
    std::string text,
    std::vector<std::string> handles) {
    if (shutdown_) {
        return {.notice = "Request could not be dispatched"};
    }
    if (is_generating()) {
        return busy_notice();
    }

    const std::shared_ptr<const Workspace> current = workspace();
    const WorkspaceForum& forum = *current->find_forum(identity_.forum_id);
    std::vector<CharacterMetadata> targets;
    if (handles.empty()) {
        targets.reserve(forum.members.size());
        for (const WorkspaceForumMember& member : forum.members) {
            const CharacterMetadata* const character =
                current->find_forum_character(
                    identity_.forum_id, member.character_id);
            if (character == nullptr) {
                throw std::logic_error(
                    "Forum member has no workspace character");
            }
            targets.push_back(*character);
        }
    } else {
        std::unordered_set<ParticipantId> distinct;
        targets.reserve(handles.size());
        for (const std::string& handle : handles) {
            const HandleResolution resolution =
                current->resolve_forum_handle(identity_.forum_id, handle);
            if (resolution.match != HandleMatch::resolved) {
                return {
                    .notice = format_handle_resolution_notice(
                        handle, resolution, *current, identity_.forum_id),
                };
            }
            if (!distinct.insert(resolution.character->id).second) {
                return {
                    .notice = format_duplicate_character_notice(
                        resolution.character->display_name),
                };
            }
            targets.push_back(*resolution.character);
        }
    }
    return start_resolved_multicast(author_id, std::move(text), std::move(targets));
}

ControllerUpdate SessionController::start_resolved_multicast(
    std::string_view author_id,
    std::string text,
    std::vector<CharacterMetadata> targets) {
    if (text.empty()) {
        return {.notice = "Multicast prompt is empty"};
    }
    if (targets.empty()) {
        return {.notice = "Multicast has no targets"};
    }

    ControllerUpdate update;
    std::optional<EntryIdentity> author = resolve_author(author_id, update);
    if (!author) return update;

    // Capture once so every child uses the same model history.
    SharedModelHistory history =
        std::make_shared<const ModelHistory>(
            transcript_.model_history());

    update.input_consumed = true;
    start_generation(
        std::move(*author),
        std::move(text),
        std::move(targets),
        std::move(history),
        update);
    return update;
}

ControllerUpdate SessionController::set_default_character_by_id(std::string_view id) {
    if (is_generating()) {
        return busy_notice();
    }
    // This typed action submits no editor text, so it never clears a draft.
    ControllerUpdate update;
    if (id == null_agent_handle) {
        default_character_id_ = std::string(null_agent_handle);
        require_snapshot(update);
        update.notice =
            "Self-notes (@-) — messages are saved to the transcript but not"
            " sent to a model. Choose a character to resume.";
        return update;
    }
    const std::shared_ptr<const Workspace> current = workspace();
    const CharacterMetadata* character =
        current->find_forum_character(identity_.forum_id, id);
    if (!character) {
        update.notice = "Unknown character";
        return update;
    }
    default_character_id_ = character->id;
    require_snapshot(update);
    update.notice = "Default character is now " + character->display_name;
    return update;
}

ControllerUpdate SessionController::request_stop() {
    ControllerUpdate update;
    if (!generation_) {
        update.notice = "No generation is active";
        return update;
    }

    if (!generation_->cancellation_requested) {
        log_info("Session generation cancellation requested");
        generation_->cancellation_requested = true;
        cancel_generation_requests();
        // Later multicast targets never acquired durable turns. Drop their
        // queues immediately; Providers retains their workers until curl has
        // observed cancellation and released its transport resources.
        generation_->requests.resize(generation_->foreground_index + 1);
        require_snapshot(update);
    }
    update.notice = "Stopping generation...";
    return update;
}

void SessionController::rename(std::string_view label) {
    validate_session_label(label);
    persist("rename session", [this, label] { journal_.rename(label); });
}

ControllerUpdate SessionController::handle_generation_event(GenerationEvent event) {
    ControllerUpdate update;
    std::visit(
        [this, &update](const auto& value) { apply(value, update); },
        event);
    return update;
}

void SessionController::apply(const GenerationEventDelta& event, ControllerUpdate& update) {
    if (!matches(event.request_id) || event.text.empty()) {
        return;
    }
    if (event.kind == GenerationDeltaKind::answer) {
        append_answer_text(
            filter_source_references(filter_answer_timestamp(event.text)),
            update);
        return;
    }
    // The first reasoning chunk establishes visible request state. Later
    // reasoning is a pure append even after answering began; the frontend's
    // transport decides whether that target switch needs a snapshot.
    const bool structural = active_->phase == ResponsePhase::waiting;
    const RequestId request_id = active_->request_id;
    active_->reasoning_text.append(event.text);
    if (structural) {
        active_->phase = ResponsePhase::reasoning;
        require_snapshot(update);
        return;
    }
    merge(update, {.state = TextAppend{ReasoningTextTarget{request_id}, event.text}});
}

void SessionController::append_answer_text(
    std::string text,
    ControllerUpdate& update) {
    if (text.empty()) return;

    // Opening the response entry also changes the phase, so only growth of an
    // already-answering entry is a pure append.
    const bool structural = active_->phase != ResponsePhase::answering;
    if (structural) {
        TranscriptEntry opened = response_entry(EntryStatus::streaming);
        active_->response_created_at = opened.created_at;
        transcript_.begin_entry(std::move(opened));
    }
    const EntryId entry_id = active_->response_entry_id;
    transcript_.append_answer(entry_id, text);
    active_->phase = ResponsePhase::answering;
    if (structural) {
        require_snapshot(update);
        return;
    }
    merge(update, {.state = TextAppend{EntryTextTarget{entry_id}, std::move(text)}});
}

void SessionController::apply(const GenerationCompleted& event, ControllerUpdate& update) {
    if (!matches(event.request_id)) {
        return;
    }
    flush_pending_answer_text(update);
    // A source-only response has no usable answer after filtering and follows
    // the existing empty-response failure path; completed entries cannot be empty.
    if (active_->phase != ResponsePhase::answering) {
        fail_active_response(
            "Generation finished without answer content", active_->character_id, update);
        finish_generation_run(update);
        return;
    }
    const TranscriptEntry response =
        response_entry(EntryStatus::complete);
    persist(
        request_action(
            "persist generation result for",
            event.request_id,
            active_->character_display_name),
        [this, &event, &response] {
            journal_.complete_turn(event.request_id, response);
        });
    finish_response_entry(EntryStatus::complete);
    active_.reset();
    require_snapshot(update);
    update.notice = "";
    finish_generation_run(update);
}

void SessionController::apply(const GenerationCancelled& event, ControllerUpdate& update) {
    if (!matches(event.request_id)) {
        return;
    }
    flush_pending_answer_text(update);
    if (active_->phase == ResponsePhase::answering) {
        const TranscriptEntry response =
            response_entry(EntryStatus::cancelled);
        persist(
            request_action(
                "persist cancellation of",
                event.request_id,
                active_->character_display_name),
            [this, &event, &response] {
                journal_.cancel_turn(event.request_id, response);
            });
        finish_response_entry(EntryStatus::cancelled);
    } else {
        persist(
            request_action(
                "persist cancellation of",
                event.request_id,
                active_->character_display_name),
            [this, &event] {
                journal_.cancel_turn(event.request_id, std::nullopt);
            });
    }
    active_.reset();
    require_snapshot(update);
    update.notice = std::string(generation_stopped_notice);
    finish_generation_run(update);
}

void SessionController::apply(const GenerationFailed& event, ControllerUpdate& update) {
    if (matches(event.request_id)) {
        fail_active_response(event.message, active_->character_id, update);
        finish_generation_run(update);
    }
}

std::string SessionController::filter_answer_timestamp(std::string_view text) {
    if (active_->answer_timestamp_state == AnswerTimestampState::passthrough) {
        return std::string(text);
    }

    active_->pending_answer_text.append(text);
    if (active_->answer_timestamp_state == AnswerTimestampState::checking) {
        const TimestampPrefixMatch match =
            match_timestamp_prefix(active_->pending_answer_text);
        if (match.result == TimestampPrefixResult::incomplete) {
            return {};
        }
        if (match.result == TimestampPrefixResult::rejected) {
            active_->answer_timestamp_state = AnswerTimestampState::passthrough;
            return std::exchange(active_->pending_answer_text, {});
        }
        active_->pending_answer_text.erase(0, match.end);
        active_->answer_timestamp_state =
            AnswerTimestampState::skipping_whitespace;
    }

    const auto content = std::ranges::find_if_not(
        active_->pending_answer_text,
        is_space);
    active_->pending_answer_text.erase(
        active_->pending_answer_text.begin(), content);
    if (active_->pending_answer_text.empty()) {
        return {};
    }
    active_->answer_timestamp_state = AnswerTimestampState::passthrough;
    return std::exchange(active_->pending_answer_text, {});
}

std::string SessionController::filter_source_references(std::string_view text) {
    std::string& pending = active_->pending_source_reference;
    pending.append(text);
    const std::size_t safe = complete_source_reference_prefix(pending);
    std::string result = remove_source_references(
        std::string_view(pending).substr(0, safe));
    pending.erase(0, safe);
    return result;
}

void SessionController::flush_pending_answer_text(ControllerUpdate& update) {
    if (active_->answer_timestamp_state == AnswerTimestampState::checking
        && !active_->pending_answer_text.empty()) {
        active_->answer_timestamp_state = AnswerTimestampState::passthrough;
        append_answer_text(
            filter_source_references(
                std::exchange(active_->pending_answer_text, {})),
            update);
    }
    append_answer_text(
        std::exchange(active_->pending_source_reference, {}), update);
}

void SessionController::fail_active_response(
    std::string message,
    ParticipantId participant_id,
    ControllerUpdate& update) {
    TranscriptEntry error = make_error_entry(
        next_entry_id_++,
        std::move(message),
        active_->request_id,
        std::move(participant_id));
    log_error("Session generation failed");
    persist(
        request_action(
            "persist failure of",
            active_->request_id,
            active_->character_display_name),
        [this, &error] {
            journal_.fail_turn(active_->request_id, error);
        });
    if (active_->phase == ResponsePhase::answering) {
        transcript_.discard_entry(active_->response_entry_id);
    }
    transcript_.add_entry(std::move(error));
    active_.reset();
    require_snapshot(update);
    update.notice = "Generation failed";
}

void SessionController::finish_response_entry(EntryStatus status) {
    transcript_.finish_entry(active_->response_entry_id, status);
}

TranscriptEntry SessionController::response_entry(EntryStatus status) const {
    std::string text;
    if (active_->phase == ResponsePhase::answering) {
        text = transcript_.open_entry_text(active_->response_entry_id);
    }
    TranscriptEntry entry = make_character_entry(
        active_->response_entry_id,
        active_->character_id,
        active_->character_display_name,
        std::move(text),
        status,
        active_->request_id);
    // The record the journal stores must carry the stamp the live streaming
    // entry opened with, not the time this terminal record was built.
    if (active_->response_created_at != 0) {
        entry.created_at = active_->response_created_at;
    }
    return entry;
}

bool SessionController::matches(RequestId request_id) const {
    return active_ && active_->request_id == request_id;
}

ControllerEventBatch SessionController::receive_events(std::size_t max_events) {
    if (max_events == 0) {
        throw std::invalid_argument("Generation event batch size must be positive");
    }
    ControllerUpdate update;
    if (shutdown_ && !generation_) {
        update.session_ended = true;
        return {.update = std::move(update)};
    }
    GenerationEvent event = GenerationCompleted{};
    std::size_t processed = 0;
    while (generation_ && active_ && processed < max_events) {
        const ChannelReadStatus status = generation_->requests[
            generation_->foreground_index]->try_receive(event);
        if (status != ChannelReadStatus::value) {
            break;
        }
        merge(update, handle_generation_event(std::move(event)));
        ++processed;
    }
    return {
        .update = std::move(update),
        .full = processed == max_events,
    };
}

void SessionController::shutdown() {
    if (shutdown_) {
        return;
    }
    log_info("Session controller shutting down");
    shutdown_ = true;
    try {
        if (generation_) {
            generation_->cancellation_requested = true;
            cancel_generation_requests();
        }
        if (active_) {
            ControllerUpdate ignored;
            apply(GenerationCancelled{active_->request_id}, ignored);
        } else {
            generation_.reset();
        }
    } catch (...) {
        generation_.reset();
        throw;
    }
}

} // namespace cha
