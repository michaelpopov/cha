#pragma once

#include "conversation.h"

#include <string>
#include <string_view>
#include <vector>

namespace cha {

// Represents one role/content record in the OpenAI-compatible request sent by an agent.
struct AgentMessage {
    std::string role;
    std::string content;

    bool operator==(const AgentMessage&) const = default;
};

// Projects shared conversation records into the role-based history expected by one agent.
[[nodiscard]] std::vector<AgentMessage> build_agent_context(
    const ConversationSnapshot& conversation,
    std::string_view system_prompt,
    std::string_view agent_name);

} // namespace cha
