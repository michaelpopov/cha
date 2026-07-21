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

    for (const ConversationEntry& entry : entries) {
        if (open_entry_id && *open_entry_id == entry.id) {
            continue;
        }
        if (entry.kind == EntryKind::notice || entry.kind == EntryKind::error) {
            continue;
        }
        if (entry.kind == EntryKind::human) {
            if (!entry.request_id || !failed_requests.contains(*entry.request_id)) {
                write_message(writer, AgentRole::user, entry.text);
            }
            continue;
        }
        if (entry.status != CompletionStatus::complete || entry.text.empty()) {
            continue;
        }

        writer.begin_message(AgentRole::assistant);
        if (entry.participant_id != agent_id) {
            writer.append_content(entry.participant_id);
            writer.append_content(": ");
        }
        writer.append_content(entry.text);
        writer.end_message();
    }
}

} // namespace cha
