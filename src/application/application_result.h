#pragma once
#include "session/session_change.h"
#include "session/session_identity.h"
#include <optional>
#include <string>
#include <vector>
namespace cha {
// Frontend-neutral outcome from the application coordinator or dispatcher.
struct ApplicationList { std::string title; std::vector<std::string> rows; };
struct ApplicationResult {
    bool input_consumed{};
    bool persona_changed{};
    bool session_changed{};
    bool exit_requested{};
    std::optional<std::string> notice;
    std::optional<ApplicationList> list;
    std::optional<SessionDescriptor> descriptor;
    SessionChange session;
};
} // namespace cha
