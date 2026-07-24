#pragma once

#include <string>

namespace cha {

enum class ResponsePhase {
    waiting,
    reasoning,
    answering,
};

struct GenerationStatus {
    bool active{};
    std::string agent_name;
    ResponsePhase phase{ResponsePhase::waiting};
};

} // namespace cha
