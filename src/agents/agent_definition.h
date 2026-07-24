#pragma once

#include "agents/config.h"

#include <string>

namespace cha {

// Holds the complete startup configuration and effective prompt for one agent.
struct AgentDefinition {
    Config config;
    std::string system_prompt;
};

} // namespace cha
