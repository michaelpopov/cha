#include "agent_protocol.h"

#include <stdexcept>

namespace cha {

void validate_completion_request(
    const CompletionRequest& request,
    std::string_view expected_agent_id) {

    if (request.request_id == 0) {
        throw std::invalid_argument("Completion request ID must be positive");
    }
    const std::string_view target = request.prompt.addressed_to;
    if (target != expected_agent_id) {
        throw std::invalid_argument(
            "Completion request targets agent '" + std::string(target)
            + "', not '" + std::string(expected_agent_id) + "'");
    }
    if (request.conversation_revision == 0) {
        throw std::invalid_argument("Completion request conversation revision must be positive");
    }

    validate_conversation_entry(request.prompt);
    if (request.prompt.kind != EntryKind::human
        || request.prompt.participant_id != human_participant_id
        || !request.prompt.request_id
        || *request.prompt.request_id != request.request_id) {
        throw std::invalid_argument("Completion request requires a matching typed human prompt");
    }
}

void validate_completion_context(
    const CompletionRequest& request,
    const ConversationReadView& conversation) {
    if (conversation.revision() != request.conversation_revision) {
        throw std::invalid_argument(
            "Completion request conversation revision changed before dispatch");
    }
    if (conversation.open_entry_id()) {
        throw std::invalid_argument(
            "Completion request cannot be built while a response entry is open");
    }
    const std::span<const ConversationEntry> entries = conversation.entries();
    if (entries.empty()) {
        throw std::invalid_argument(
            "Completion request conversation does not contain its prompt");
    }
    if (entries.back() != request.prompt) {
        throw std::invalid_argument(
            "Completion request prompt is not the latest conversation entry");
    }
}

} // namespace cha
