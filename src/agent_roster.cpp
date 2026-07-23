#include "agent_roster.h"

#include "agent_identity.h"
#include "text.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace cha {
namespace {

bool ascii_iequals(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        char left_character = left[index];
        char right_character = right[index];
        if (left_character >= 'A' && left_character <= 'Z') {
            left_character = static_cast<char>(left_character - 'A' + 'a');
        }
        if (right_character >= 'A' && right_character <= 'Z') {
            right_character = static_cast<char>(right_character - 'A' + 'a');
        }
        if (left_character != right_character) {
            return false;
        }
    }
    return true;
}

bool starts_with_folded(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size()
        && ascii_iequals(value.substr(0, prefix.size()), prefix);
}

std::string_view trim_punctuation(std::string_view handle) {
    while (!handle.empty()
           && std::string_view(",.;:!?").find(handle.back())
               != std::string_view::npos) {
        handle.remove_suffix(1);
    }
    return handle;
}

} // namespace

AgentRoster::AgentRoster(std::vector<AgentInfo> agents)
    : agents_(std::move(agents)) {
    if (agents_.empty()) {
        throw std::invalid_argument("Agent roster cannot be empty");
    }
    std::unordered_set<std::string> ids;
    std::unordered_set<std::string> names;
    for (const AgentInfo& agent : agents_) {
        validate_agent_id(agent.id);
        validate_agent_name(agent.name);
        if (!ids.insert(agent.id).second) {
            throw std::invalid_argument(
                "Agent roster has duplicate agent ID '" + agent.id + "'");
        }
        if (!names.insert(fold_ascii(agent.name)).second) {
            throw std::invalid_argument(
                "Agent roster has duplicate agent name '" + agent.name + "'");
        }
    }
}

const std::vector<AgentInfo>& AgentRoster::agents() const noexcept { return agents_; }
const AgentInfo& AgentRoster::first() const { return agents_.front(); }

const AgentInfo* AgentRoster::find(std::string_view id) const {
    const auto found = std::find_if(
        agents_.begin(), agents_.end(),
        [id](const AgentInfo& agent) { return agent.id == id; });
    return found == agents_.end() ? nullptr : &*found;
}

HandleResolution AgentRoster::resolve_handle(std::string_view handle) const {
    if (handle.empty()) {
        return {};
    }
    const auto named = [this](std::string_view value) -> const AgentInfo* {
        const auto found = std::find_if(agents_.begin(), agents_.end(), [value](const AgentInfo& agent) {
            return ascii_iequals(agent.name, value);
        });
        return found == agents_.end() ? nullptr : &*found;
    };
    if (const AgentInfo* agent = named(handle)) {
        return {HandleMatch::resolved, agent, {}};
    }
    const std::string_view trimmed = trim_punctuation(handle);
    if (trimmed != handle) {
        if (const AgentInfo* agent = named(trimmed)) {
            return {HandleMatch::resolved, agent, {}};
        }
    }
    if (trimmed.empty()) {
        return {};
    }
    std::vector<const AgentInfo*> candidates;
    for (const AgentInfo& agent : agents_) {
        if (starts_with_folded(agent.name, trimmed)) {
            candidates.push_back(&agent);
        }
    }
    if (candidates.size() == 1) {
        return {HandleMatch::resolved, candidates.front(), {}};
    }
    if (candidates.empty()) {
        return {};
    }
    return {HandleMatch::ambiguous, nullptr, std::move(candidates)};
}

std::string AgentRoster::handle_list() const {
    std::string result;
    for (const AgentInfo& agent : agents_) {
        if (!result.empty()) result += ", ";
        result += "@" + agent.name;
    }
    return result;
}

} // namespace cha
