#include "agent_context.h"

#include <unordered_set>

namespace cha {

std::vector<AgentMessage> build_agent_context(
    const ConversationSnapshot& conversation,
    std::string_view system_prompt,
    std::string_view agent_id) {

    std::vector<AgentMessage> result;
    if (!system_prompt.empty()) {
        result.push_back({"system", std::string(system_prompt)});
    }

    std::unordered_set<RequestId> failed_requests;
    for (const ConversationEntry& entry : conversation.entries) {
        if (entry.kind == EntryKind::error && entry.request_id) {
            failed_requests.insert(*entry.request_id);
        }
    }

    for (const ConversationEntry& entry : conversation.entries) {
        if (conversation.open_entry_id && *conversation.open_entry_id == entry.id) {
            continue;
        }
        if (entry.kind == EntryKind::notice || entry.kind == EntryKind::error) {
            continue;
        }
        if (entry.kind == EntryKind::human) {
            if (!entry.request_id || !failed_requests.contains(*entry.request_id)) {
                result.push_back({"user", entry.text});
            }
            continue;
        }
        if (entry.status != CompletionStatus::complete || entry.text.empty()) {
            continue;
        }
        const std::string content = entry.participant_id == agent_id
            ? entry.text
            : entry.participant_id + ": " + entry.text;
        result.push_back({"assistant", content});
    }

    return result;
}

} // namespace cha
