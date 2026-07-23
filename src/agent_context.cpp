#include "agent_context.h"

#include <unordered_set>

namespace cha {
namespace {

void write_message(
    AgentContextWriter& writer,
    AgentRole role,
    std::string_view content) {
    writer.begin_message(role);
    writer.append_content(content);
    writer.end_message();
}

} // namespace

void write_agent_context(
    std::span<const ConversationEntry> entries,
    std::optional<EntryId> open_entry_id,
    std::string_view system_prompt,
    std::string_view agent_id,
    AgentContextWriter& writer) {
    if (!system_prompt.empty()) {
        write_message(writer, AgentRole::system, system_prompt);
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

    bool message_open = false;
    AgentRole open_role = AgentRole::system;
    bool previous_foreign = false;
    for (const ConversationEntry& entry : entries) {
        if (!projectable(entry)) continue;
        const bool foreign = entry.kind == EntryKind::agent && entry.participant_id != agent_id;
        const AgentRole role = entry.kind == EntryKind::human || foreign
            ? AgentRole::user : AgentRole::assistant;
        const bool coalesce = message_open && role == AgentRole::user && open_role == AgentRole::user
            && (previous_foreign || foreign);
        if (message_open && !coalesce) {
            writer.end_message();
        }
        if (!coalesce) {
            writer.begin_message(role);
            message_open = true;
            open_role = role;
        } else {
            writer.append_content("\n\n");
        }

        if (entry.kind == EntryKind::human) {
            if (attributed) {
                writer.append_content("User: ");
                if (entry.addressed_to != agent_id) {
                    writer.append_content("[to ");
                    writer.append_content(entry.addressed_to_name);
                    writer.append_content("] ");
                }
            }
            writer.append_content(entry.text);
        } else if (foreign) {
            writer.append_content(entry.display_name);
            writer.append_content(": ");
            writer.append_content(entry.text);
        } else {
            writer.append_content(entry.text);
        }
        previous_foreign = foreign;
    }
    if (message_open) writer.end_message();
}

} // namespace cha
