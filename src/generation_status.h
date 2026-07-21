#pragma once

#include <string>

namespace cha {

struct GenerationStatus {
    bool active{};
    std::string agent_name;
};

} // namespace cha
