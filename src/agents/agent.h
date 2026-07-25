#pragma once

#include "agents/config.h"
#include "conversation/conversation.h"
#include "util/event_channel.h"

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cha {

// A persona loaded and ready to run: its Config plus the effective system prompt, which combines
// the persona's own prompt with the instructions of the room it was loaded for.
struct AgentDefinition {
    Config config;
    std::string system_prompt;
};

// The public face of an agent, safe to hand to interfaces and notices. It carries the identity and
// capability fields needed for display and @handle resolution, and never any connection secret.
struct AgentInfo {
    std::string id;
    std::string name;
    std::string model;
    std::string api;
    bool streaming{};
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

// Loads persona configuration and combines its system prompt with room instructions.
AgentDefinition load_agent_definition(
    const std::filesystem::path& persona_directory,
    const std::filesystem::path& room_directory);

std::vector<AgentDefinition> load_agent_definitions(
    const std::vector<std::filesystem::path>& persona_directories,
    const std::filesystem::path& room_directory);

void validate_agent_id(std::string_view id);
void validate_agent_name(std::string_view name);

// Projects typed transcript entries into protocol roles for one stable agent participant ID.
std::vector<AgentMessage> project_agent_context(
    std::span<const ConversationEntry> entries,
    std::optional<EntryId> open_entry_id,
    std::string_view system_prompt,
    std::string_view agent_id);

// One unit of work for AgentRegistry: the human ConversationEntry to answer, plus the request ID
// that correlates every event of that run back to it.
struct CompletionRequest {
    RequestId request_id{};
    ConversationEntry prompt;
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
using AgentEventChannel = EventChannel<AgentEvent>;

} // namespace cha
