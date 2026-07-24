#pragma once

#include <string>
#include <string_view>

namespace cha {

inline constexpr std::string_view generation_in_progress_notice =
    "Generation in progress; use /stop, Esc, or Ctrl-C";

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
