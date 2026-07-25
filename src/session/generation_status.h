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

// What a front end needs to show about the turn in progress: whether one is running, which agent
// is answering, and how far it has got. Reported by ResponseController through SessionController.
struct GenerationStatus {
    bool active{};
    std::string agent_name;
    ResponsePhase phase{ResponsePhase::waiting};
};

} // namespace cha
