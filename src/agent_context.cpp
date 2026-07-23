#include "agent_context.h"

#include <unordered_set>

namespace cha {

std::vector<AgentMessage> project_agent_context(
    std::span<const ConversationEntry> entries,
    std::optional<EntryId> open_entry_id,
    std::string_view system_prompt,
    std::string_view agent_id) {
    std::vector<AgentMessage> messages;
    if (!system_prompt.empty()) {
        messages.push_back({AgentRole::system, std::string(system_prompt)});
    }

    std::unordered_set<RequestId> failed_requests;
    for (const ConversationEntry& entry : entries) {
        if (entry.kind == EntryKind::error && entry.request_id) {
            failed_requests.insert(*entry.request_id);
        }
    }

    const auto projectable = [&failed_requests, open_entry_id](const ConversationEntry& entry) {
        if (open_entry_id && *open_entry_id == entry.id) return false;
        if (entry.kind == EntryKind::notice || entry.kind == EntryKind::error) return false;
        if (entry.kind == EntryKind::human) return !entry.request_id || !failed_requests.contains(*entry.request_id);
        return entry.status == CompletionStatus::complete && !entry.text.empty();
    };

    bool attributed = false;
    for (const ConversationEntry& entry : entries) {
        if (!projectable(entry)) continue;
        attributed = attributed || (entry.kind == EntryKind::agent && entry.participant_id != agent_id)
            || (entry.kind == EntryKind::human && entry.addressed_to != agent_id);
    }

    bool previous_foreign = false;
    for (const ConversationEntry& entry : entries) {
        if (!projectable(entry)) continue;
        const bool foreign = entry.kind == EntryKind::agent && entry.participant_id != agent_id;
        const AgentRole role = entry.kind == EntryKind::human || foreign
            ? AgentRole::user : AgentRole::assistant;
        const bool coalesce = !messages.empty()
            && role == AgentRole::user
            && messages.back().role == AgentRole::user
            && (previous_foreign || foreign);
        if (!coalesce) {
            messages.push_back({role, {}});
        } else {
            messages.back().content.append("\n\n");
        }

        std::string& content = messages.back().content;
        if (entry.kind == EntryKind::human) {
            if (attributed) {
                content.append("User: ");
                if (entry.addressed_to != agent_id) {
                    content.append("[to ");
                    content.append(entry.addressed_to_name);
                    content.append("] ");
                }
            }
            content.append(entry.text);
        } else if (foreign) {
            content.append(entry.display_name);
            content.append(": ");
            content.append(entry.text);
        } else {
            content.append(entry.text);
        }
        previous_foreign = foreign;
    }
    return messages;
}

} // namespace cha
