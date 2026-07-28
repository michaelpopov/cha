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
    stopping,
};

// What a front end needs to show about execution or abort cleanup in progress.
// Reasoning is ephemeral presentation state: it is cleared at the terminal
// event and never enters the Transcript or session database.
struct GenerationStatus {
    bool active{};
    std::string agent_name;
    ResponsePhase phase{ResponsePhase::waiting};
    std::string reasoning_text;

    bool operator==(const GenerationStatus&) const = default;
};

} // namespace cha
