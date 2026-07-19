#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace cha {

enum class CommandKind {
    text,
    clear,
    info,
    model,
    stop,
    exit,
    unknown,
};

struct Command {
    CommandKind kind{CommandKind::text};
    std::string argument;
};

[[nodiscard]] Command parse_command(std::string_view input);
[[nodiscard]] std::optional<std::string> confirmed_model(std::string_view reply);

} // namespace cha
