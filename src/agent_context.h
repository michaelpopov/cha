#pragma once

#include "conversation.h"

#include <string>
#include <string_view>
#include <vector>

namespace cha {

// Represents one portable protocol-role record for an inference request.
struct AgentMessage {
    std::string role;
    std::string content;

    bool operator==(const AgentMessage&) const = default;
};

// Projects typed transcript entries into protocol roles for one stable agent participant ID.
[[nodiscard]] std::vector<AgentMessage> build_agent_context(
    const ConversationSnapshot& conversation,
    std::string_view system_prompt,
    std::string_view agent_id);

} // namespace cha
