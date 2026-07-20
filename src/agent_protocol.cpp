#include "agent_protocol.h"

#include <stdexcept>

namespace cha {

void validate_completion_request(
    const CompletionRequest& request,
    std::string_view expected_agent_id) {

    if (request.request_id == 0) {
        throw std::invalid_argument("Completion request ID must be positive");
    }
    if (request.agent_id != expected_agent_id) {
        throw std::invalid_argument(
            "Completion request targets agent '" + request.agent_id
            + "', not '" + std::string(expected_agent_id) + "'");
    }

    validate_conversation_entry(request.prompt);
    if (request.prompt.kind != EntryKind::human
        || !request.prompt.request_id
        || *request.prompt.request_id != request.request_id) {
        throw std::invalid_argument("Completion request requires a matching typed human prompt");
    }
}

} // namespace cha
