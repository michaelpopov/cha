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

// Holds the complete startup configuration and effective prompt for one agent.
struct AgentDefinition {
    Config config;
    std::string system_prompt;
};

// Exposes immutable agent details for local UI commands without granting access to agent internals.
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

// One materialized message in the model-visible context.
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

// Carries the new prompt and correlation data for one completion request.
struct CompletionRequest {
    RequestId request_id{};
    ConversationEntry prompt;
};

// Carries one streamed response fragment for an identified request.
struct AgentDelta {
    RequestId request_id{};
    CompletionDeltaKind kind{CompletionDeltaKind::answer};
    std::string text;
};

// Marks successful completion of an identified request.
struct AgentCompleted {
    RequestId request_id{};
};

// Marks cancellation of an identified request, possibly after response fragments were emitted.
struct AgentCancelled {
    RequestId request_id{};
};

// Reports a terminal inference failure for an identified request.
struct AgentFailed {
    RequestId request_id{};
    std::string message;
};

using AgentEvent = std::variant<AgentDelta, AgentCompleted, AgentCancelled, AgentFailed>;
using AgentEventChannel = EventChannel<AgentEvent>;

} // namespace cha
