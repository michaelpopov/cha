#pragma once

#include <string>

namespace cha {

// Exposes immutable agent details for local UI commands without granting access to agent internals.
struct AgentInfo {
    std::string id;
    std::string name;
    std::string model;
    std::string api;
    bool streaming{};
};

} // namespace cha
