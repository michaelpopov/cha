#pragma once

#include "agent_info.h"

#include <string>
#include <string_view>
#include <vector>

namespace cha {

enum class HandleMatch { resolved, unknown, ambiguous };

struct HandleResolution {
    HandleMatch match{HandleMatch::unknown};
    const AgentInfo* agent{};
    std::vector<const AgentInfo*> candidates;
};

class AgentRoster {
public:
    explicit AgentRoster(std::vector<AgentInfo> agents);

    [[nodiscard]] const std::vector<AgentInfo>& agents() const noexcept;
    [[nodiscard]] const AgentInfo& first() const;
    [[nodiscard]] const AgentInfo* find(std::string_view id) const;
    [[nodiscard]] HandleResolution resolve_handle(std::string_view handle) const;
    [[nodiscard]] std::string handle_list() const;

private:
    std::vector<AgentInfo> agents_;
};

} // namespace cha
