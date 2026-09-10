#pragma once

#include <string>
#include <string_view>

namespace cha::web {

enum class CommandKind {
    text,
    mcast,
    unknown,
};

// One line of persona input after parsing: the command it names (plain text when it names none) and
// the text that followed it. Produced by
// parse_command() and turned into controller calls by handle_text_input().
struct Command {
    CommandKind kind{CommandKind::text};
    std::string argument;
};

Command parse_command(std::string_view input);
std::string command_names();

} // namespace cha::web
