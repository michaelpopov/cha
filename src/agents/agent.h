#pragma once

#include "agents/config.h"
#include "agents/user.h"
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
    "user", "system", "error", "human", "assistant", "agent", "you",
};

// A persona loaded and ready to run: its Config plus the effective system
// prompt, which combines persona instructions, forum settings, the user
// roster, and generated forum context.
struct AgentDefinition {
    Config config;
    std::string system_prompt;
};

// The stable identity of one configured persona.
struct PersonaInfo {
    std::string id;
    std::string name;
};

// Public operational information about one initialized agent backend. It is safe to show in
// diagnostics and never carries connection secrets.
struct AgentRuntimeInfo {
    PersonaInfo persona;
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
    PersonaInfo target;
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
enum class AgentRole {
    system,
    user,
    assistant,
};

// One model-visible message, produced from transcript entries by project_agent_context(). Roles
// stay semantic here so that only the transport layer knows their wire spelling.
struct AgentMessage {
    AgentRole role{};
    std::string content;

    bool operator==(const AgentMessage&) const = default;
};

// Loads the forum's persona configurations in order and assembles each effective system prompt.
std::vector<AgentDefinition> load_agent_definitions(
    const std::vector<std::filesystem::path>& persona_directories,
    const std::filesystem::path& forum_directory,
    std::string_view forum_display_name,
    const UserRoster& users,
    std::optional<std::filesystem::path> base_config_path = std::nullopt);

void validate_persona_id(std::string_view id);
void validate_persona_name(std::string_view name);

// Projects typed transcript entries into protocol roles for one stable agent participant ID.
std::vector<AgentMessage> project_agent_context(
    std::span<const TranscriptEntry> entries,
    std::optional<EntryId> open_entry_id,
    OffrecordSpan offrecord_span,
    std::string_view system_prompt,
    std::string_view agent_id);

// Projects immutable history and appends the run's addressed prompt exactly
// once as a user message.
std::vector<AgentMessage> project_agent_context(
    const CompletionInput& input,
    std::string_view system_prompt);

// One fragment of streamed provider output, classified as private reasoning or answer text.
// CompletionBackend emits this without request identity; AgentRegistry attaches that identity
// when it publishes an AgentDelta.
enum class CompletionDeltaKind {
    reasoning,
    answer,
};

struct CompletionDelta {
    CompletionDeltaKind kind{CompletionDeltaKind::answer};
    std::string text;
};

// The four AgentEvent alternatives below report the progress of one accepted request. A request
// emits any number of deltas and then exactly one terminal event.

// A streamed output fragment, now correlated with the request that produced it.
struct AgentDelta {
    RequestId request_id{};
    CompletionDeltaKind kind{CompletionDeltaKind::answer};
    std::string text;
};

// Terminal event: the request produced its full response.
struct AgentCompleted {
    RequestId request_id{};
};

// Terminal event: the request stopped on request, possibly after emitting fragments.
struct AgentCancelled {
    RequestId request_id{};
};

// Terminal event: the request ended in a transport or protocol error, with a message for the user.
struct AgentFailed {
    RequestId request_id{};
    std::string message;
};

using AgentEvent = std::variant<AgentDelta, AgentCompleted, AgentCancelled, AgentFailed>;

} // namespace cha
