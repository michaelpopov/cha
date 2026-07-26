#pragma once

#include "session/workspace.h"

#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <variant>

namespace cha {

enum class ColorMode {
    automatic,
    always,
    never,
};

struct ConsoleOptions {
    std::string room;
    std::string session_id;
    std::optional<std::string> new_label;
    bool list_rooms{};
    bool list_sessions{};
    ColorMode color{ColorMode::automatic};
};

struct ArgumentError {
    std::string message;
    int exit_code{2};
};

std::variant<ConsoleOptions, ArgumentError> parse_console_arguments(
    int argc,
    const char* const* argv);

void write_room_listing(const Workspace& workspace, std::ostream& out);
void write_session_listing(
    const Workspace& workspace,
    const std::string& room,
    std::ostream& out);

[[nodiscard]] std::unique_ptr<SessionController> open_console_session(
    const Workspace& workspace,
    const ConsoleOptions& options);

} // namespace cha
