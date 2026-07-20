#pragma once

#include <string>
#include <string_view>

namespace cha {

enum class CommandKind {
    text,
    clear,
    info,
    stop,
    exit,
    unknown,
};

// Represents one parsed slash command and its optional argument for command dispatch.
struct Command {
    CommandKind kind{CommandKind::text};
    std::string argument;
};

[[nodiscard]] Command parse_command(std::string_view input);

} // namespace cha
