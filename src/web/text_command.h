#pragma once

#include <string>
#include <string_view>

namespace cha::web {

enum class CommandKind {
    text,
    clear,
    hide_on,
    hide,
    hide_off,
    mcast,
    info,
    stop,
    exit,
    characters,
    set_default,
    unknown,
};

// One line of persona input after parsing: the command it names (plain text when it names none), the
// text that followed it, and the handle when the line addresses a character. Produced by
// parse_command() and turned into controller calls by handle_text_input().
struct Command {
    CommandKind kind{CommandKind::text};
    std::string argument;
    std::string handle;
};

Command parse_command(std::string_view input);
std::string command_names();

} // namespace cha::web
