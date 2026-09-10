#include "web/text_command.h"

#include "util/text.h"

#include <array>

namespace cha::web {
namespace {

struct CommandDescriptor {
    std::string_view name;
    CommandKind kind;
};

constexpr std::array descriptors{
    CommandDescriptor{"/mcast", CommandKind::mcast},
};

} // namespace

Command parse_command(std::string_view input) {
    if (!input.starts_with('/')) {
        return {};
    }

    const std::size_t separator = find_whitespace(input);
    const std::string_view name = input.substr(0, separator);
    const std::string argument =
        separator == std::string_view::npos ? "" : std::string(trim_view(input.substr(separator)));

    for (const CommandDescriptor& descriptor : descriptors) {
        if (name == descriptor.name) {
            return {descriptor.kind, argument};
        }
    }
    return {CommandKind::unknown, argument};
}

std::string command_names() {
    std::string result;
    for (const CommandDescriptor& descriptor : descriptors) {
        if (!result.empty()) result += ", ";
        result += descriptor.name;
    }
    return result;
}

} // namespace cha::web
