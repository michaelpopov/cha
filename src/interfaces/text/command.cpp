#include "interfaces/text/command.h"

#include "util/text.h"

namespace cha {

Command parse_command(std::string_view input) {
    if (!input.starts_with('/')) {
        return {};
    }

    if (input.starts_with("/@")) {
        const std::size_t separator = find_whitespace(input);
        const std::string_view handle = input.substr(2, separator == std::string_view::npos ? std::string_view::npos : separator - 2);
        const std::string argument = separator == std::string_view::npos ? "" : std::string(trim_view(input.substr(separator)));
        return {CommandKind::set_default, argument, std::string(handle)};
    }

    const std::size_t separator = find_whitespace(input);
    const std::string_view name = input.substr(0, separator);
    const std::string argument =
        separator == std::string_view::npos ? "" : std::string(trim_view(input.substr(separator)));

    if (name == "/clear") {
        return {CommandKind::clear, argument};
    }
    if (name == "/info") {
        return {CommandKind::info, argument};
    }
    if (name == "/stop") {
        return {CommandKind::stop, argument};
    }
    if (name == "/exit") {
        return {CommandKind::exit, argument};
    }
    if (name == "/agents") {
        return {CommandKind::agents, argument};
    }
    return {CommandKind::unknown, argument};
}

} // namespace cha
