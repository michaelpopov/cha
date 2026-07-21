#pragma once

#include "conversation.h"

#include <optional>
#include <span>
#include <string_view>

namespace cha {

// Classifies model-visible message roles independently of their JSON spelling.
enum class AgentRole {
    system,
    user,
    assistant,
};

// Consumes one projected model message synchronously without owning its content.
class AgentContextWriter {
public:
    virtual ~AgentContextWriter() = default;

    virtual void begin_message(AgentRole role) = 0;
    virtual void append_content(std::string_view text) = 0;
    virtual void end_message() = 0;
};

// Projects typed transcript entries into protocol roles for one stable agent participant ID.
void write_agent_context(
    std::span<const ConversationEntry> entries,
    std::optional<EntryId> open_entry_id,
    std::string_view system_prompt,
    std::string_view agent_id,
    AgentContextWriter& writer);

} // namespace cha
