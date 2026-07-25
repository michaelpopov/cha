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
    agents,
    set_default,
    unknown,
};

// One line of user input after parsing: the command it names (plain text when it names none), the
// text that followed it, and the handle when the line addresses an agent. Produced by
// parse_command() and turned into controller calls by handle_text_input().
struct Command {
    CommandKind kind{CommandKind::text};
    std::string argument;
    std::string handle;
};

Command parse_command(std::string_view input);

} // namespace cha
