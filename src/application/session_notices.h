#pragma once

#include "agents/agent_registry.h"
#include "conversation/conversation.h"

#include <string>
#include <string_view>

namespace cha {

std::string format_handle_notice(
    std::string_view handle,
    const HandleResolution& resolution,
    const AgentRoster& roster);

std::string format_roster_notice(
    const AgentRoster& roster,
    const ParticipantId& default_agent_id);

std::string format_session_information(
    const Conversation& conversation,
    const AgentRoster& roster,
    const ParticipantId& default_agent_id);

} // namespace cha
