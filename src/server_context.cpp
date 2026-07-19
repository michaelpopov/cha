#include "server_context.h"

#include "command.h"

namespace cha {

std::vector<ServerMessage> build_server_context(
    const ConversationSnapshot& conversation,
    std::string_view system_prompt,
    std::string_view server_name) {

    std::vector<ServerMessage> result;
    if (!system_prompt.empty()) {
        result.push_back({"system", std::string(system_prompt)});
    }

    const std::size_t system_messages = result.size();
    bool skip_agent_reply = false;
    const std::size_t message_count = conversation.messages.size()
        - (conversation.message_open && !conversation.messages.empty() ? 1 : 0);

    for (std::size_t index = 0; index < message_count; ++index) {
        const ConversationMessage& message = conversation.messages[index];
        if (message.author == system_author) {
            if (result.size() > system_messages && result.back().role == "user") {
                result.pop_back();
            }
            skip_agent_reply = false;
            continue;
        }

        if (message.author == user_author) {
            const Command command = parse_command(message.text);
            if (command.kind == CommandKind::clear && command.argument.empty()) {
                result.resize(system_messages);
            }
            if (command.kind != CommandKind::text) {
                skip_agent_reply = true;
                continue;
            }

            result.push_back({"user", message.text});
            skip_agent_reply = false;
            continue;
        }

        if (skip_agent_reply) {
            skip_agent_reply = false;
            continue;
        }
        if (message.text.empty()) {
            continue;
        }

        if (message.author == server_name) {
            result.push_back({"assistant", message.text});
        } else {
            result.push_back({"user", message.author + ": " + message.text});
        }
    }

    return result;
}

} // namespace cha
