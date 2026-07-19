#pragma once

#include "conversation.h"

#include <string>
#include <string_view>
#include <vector>

namespace cha {

struct ServerMessage {
    std::string role;
    std::string content;

    bool operator==(const ServerMessage&) const = default;
};

// Project shared conversation records into the role-based history expected by one server.
[[nodiscard]] std::vector<ServerMessage> build_server_context(
    const ConversationSnapshot& conversation,
    std::string_view system_prompt,
    std::string_view server_name);

} // namespace cha
