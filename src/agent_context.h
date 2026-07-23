#pragma once

#include "conversation.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

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

// Projects typed transcript entries into protocol roles for one stable agent participant ID.
[[nodiscard]] std::vector<AgentMessage> project_agent_context(
    std::span<const ConversationEntry> entries,
    std::optional<EntryId> open_entry_id,
    std::string_view system_prompt,
    std::string_view agent_id);

} // namespace cha
