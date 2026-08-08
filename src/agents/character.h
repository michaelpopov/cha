#pragma once

#include "agents/config.h"
#include "agents/persona.h"
#include "transcript/transcript.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cha {

// Participant names no configured party may claim.
inline constexpr std::string_view reserved_participant_names[] = {
    "persona", "system", "error", "human", "assistant", "agent", "character", "you", "guest",
};

// A character loaded and ready to run. Public identity remains separate from
// the private completion-provider configuration used to execute it.
struct CharacterDefinition {
    CharacterMetadata character;
    CompletionConfig completion;
    std::string system_prompt;
};

// The two filesystem layers that form one forum member's effective character.
struct CharacterDefinitionSource {
    std::filesystem::path definition_directory;
    std::filesystem::path member_directory;
};

// Public operational information about one initialized completion backend. It
// is safe to show in diagnostics and never carries connection secrets.
struct CompletionBackendInfo {
    CharacterMetadata character;
    std::string model;
    std::string api;
    bool streaming{};
};

using SharedCompletionHistory =
    std::shared_ptr<const CompletionHistory>;

// One logical model run. It deliberately has no durable transcript entry ID:
// those IDs are assigned only when the controller activates the run.
struct RunSpec {
    RequestId request_id{};
    CharacterMetadata target;
    EntryIdentity author;
    std::string prompt_text;
};

// Complete immutable input for backend preparation. Ordinary requests and
// multicast children use the same representation.
struct CompletionInput {
    SharedCompletionHistory history;
    RunSpec run;
};

// Classifies model-visible message roles independently of their JSON spelling.
enum class CompletionRole {
    system,
    persona,
    assistant,
};

// One model-visible message, produced from transcript entries by project_completion_context(). Roles
// stay semantic here so that only the transport layer knows their wire spelling.
struct CompletionMessage {
    CompletionRole role{};
    std::string content;

    bool operator==(const CompletionMessage&) const = default;
};

// Loads the forum's character configurations in order and assembles each effective system prompt.
std::vector<CharacterDefinition> load_character_definitions(
    const std::vector<CharacterDefinitionSource>& sources,
    const std::filesystem::path& forum_directory,
    std::string_view forum_display_name,
    const PersonaRoster& personas,
    std::optional<std::filesystem::path> forum_defaults_path = std::nullopt,
    std::optional<ProviderConfig> application_provider = std::nullopt);

// Adds the standard participant and forum-context sections to definitions
// assembled by trusted non-filesystem factories.
void append_standard_prompt_context(
    std::vector<CharacterDefinition>& definitions,
    const PersonaRoster& personas);

void validate_character_id(std::string_view id);
// Validates the transport/runtime shape only. Workspace definitions must use
// validate_character_display_name(), which additionally enforces reserved names.
void validate_character_display_name_syntax(std::string_view display_name);
void validate_character_display_name(std::string_view display_name);
void validate_persona_character_collisions(
    const PersonaRoster& personas,
    const std::vector<CharacterMetadata>& definitions);

// Projects typed transcript entries into provider roles for one stable character ID.
std::vector<CompletionMessage> project_completion_context(
    std::span<const TranscriptEntry> entries,
    std::optional<EntryId> open_entry_id,
    OffrecordSpan offrecord_span,
    std::string_view system_prompt,
    std::string_view character_id);

// Projects immutable history and appends the run's addressed prompt exactly
// once as a persona message.
std::vector<CompletionMessage> project_completion_context(
    const CompletionInput& input,
    std::string_view system_prompt);

// One fragment of streamed provider output, classified as private reasoning or answer text.
// CompletionBackend emits this without request identity; the batch execution attaches that
// identity when it publishes a CompletionEventDelta.
enum class CompletionDeltaKind {
    reasoning,
    answer,
};

struct CompletionDelta {
    CompletionDeltaKind kind{CompletionDeltaKind::answer};
    std::string text;
};

// The four CompletionEvent alternatives below report the progress of one accepted request. A request
// emits any number of deltas and then exactly one terminal event.

// A streamed output fragment, now correlated with the request that produced it.
struct CompletionEventDelta {
    RequestId request_id{};
    CompletionDeltaKind kind{CompletionDeltaKind::answer};
    std::string text;
};

// Terminal event: the request produced its full response.
struct CompletionCompleted {
    RequestId request_id{};
};

// Terminal event: the request stopped on request, possibly after emitting fragments.
struct CompletionCancelled {
    RequestId request_id{};
};

// Terminal event: the request ended in a transport or protocol error, with a message for the persona.
struct CompletionFailed {
    RequestId request_id{};
    std::string message;
};

using CompletionEvent = std::variant<CompletionEventDelta, CompletionCompleted, CompletionCancelled, CompletionFailed>;

} // namespace cha
