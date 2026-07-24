#include "application/session_notices.h"

#include <sstream>

namespace cha {

std::string format_handle_notice(
    std::string_view handle,
    const HandleResolution& resolution,
    const AgentRoster& roster) {
    if (resolution.match == HandleMatch::unknown) {
        return "Unknown agent @" + std::string(handle)
            + ". Agents in this room: " + roster.handle_list();
    }
    std::string result =
        "Ambiguous agent @" + std::string(handle) + ": matches ";
    for (std::size_t i = 0; i < resolution.candidates.size(); ++i) {
        if (i) {
            result += ", ";
        }
        result += "@" + resolution.candidates[i]->name;
    }
    return result + ". Type more of the name.";
}

std::string format_roster_notice(
    const AgentRoster& roster,
    const ParticipantId& default_agent_id) {
    std::ostringstream result;
    result << "Agents in this room (" << roster.agents().size()
           << "), * marks the default.";
    result << " Any unambiguous prefix works.";
    for (const AgentInfo& agent : roster.agents()) {
        result << " | " << (agent.id == default_agent_id ? "* " : "")
               << "@" << agent.name << "  " << agent.model << "  "
               << agent.api << "  "
               << (agent.streaming ? "streaming" : "non-streaming");
    }
    return result.str();
}

std::string format_session_information(
    const Conversation& conversation,
    const AgentRoster& roster,
    const ParticipantId& default_agent_id) {
    std::ostringstream text;
    text << "Transcript entries: " << conversation.snapshot().entries.size()
         << " | " << format_roster_notice(roster, default_agent_id);
    return text.str();
}

} // namespace cha
