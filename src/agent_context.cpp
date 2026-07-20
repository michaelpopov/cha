#include "agent_context.h"

namespace cha {

std::vector<AgentMessage> build_agent_context(
    const ConversationSnapshot& conversation,
    std::string_view system_prompt,
    std::string_view agent_name) {

    std::vector<AgentMessage> result;
    if (!system_prompt.empty()) {
        result.push_back({"system", std::string(system_prompt)});
    }

    const std::size_t system_messages = result.size();
    const std::size_t message_count = conversation.messages.size()
        - (conversation.message_open && !conversation.messages.empty() ? 1 : 0);

    for (std::size_t index = 0; index < message_count; ++index) {
        const ConversationMessage& message = conversation.messages[index];
        if (message.author == system_author) {
            if (result.size() > system_messages && result.back().role == "user") {
                result.pop_back();
            }
            continue;
        }

        if (message.author == user_author) {
            result.push_back({"user", message.text});
            continue;
        }
        if (message.text.empty()) {
            continue;
        }

        if (message.author == agent_name) {
            result.push_back({"assistant", message.text});
        } else {
            result.push_back({"user", message.author + ": " + message.text});
        }
    }

    return result;
}

} // namespace cha
