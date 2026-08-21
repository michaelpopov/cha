#include "web/text_command.h"

#include "util/text.h"

#include <array>

namespace cha::web {
namespace {

enum class CommandForm {
    exact,
    handle_suffix,
};

struct CommandDescriptor {
    std::string_view name;
    CommandKind kind;
    CommandForm form{CommandForm::exact};
};

constexpr std::array descriptors{
    CommandDescriptor{"/clear", CommandKind::clear},
    CommandDescriptor{"/hide-on", CommandKind::hide_on},
    CommandDescriptor{"/hide", CommandKind::hide},
    CommandDescriptor{"/hide-off", CommandKind::hide_off},
    CommandDescriptor{"/mcast", CommandKind::mcast},
    CommandDescriptor{"/info", CommandKind::info},
    CommandDescriptor{"/characters", CommandKind::characters},
    // Compatibility alias retained for existing scripts and muscle memory.
    CommandDescriptor{"/agents", CommandKind::characters},
    CommandDescriptor{"/@", CommandKind::set_default, CommandForm::handle_suffix},
    CommandDescriptor{"/!", CommandKind::set_persona, CommandForm::handle_suffix},
    CommandDescriptor{"/style", CommandKind::session_style},
    CommandDescriptor{"/stop", CommandKind::stop},
    CommandDescriptor{"/exit", CommandKind::exit},
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
        if (descriptor.form == CommandForm::exact) {
            if (name == descriptor.name) {
                return {descriptor.kind, argument};
            }
        } else if (name.starts_with(descriptor.name)) {
            return {
                descriptor.kind,
                argument,
                std::string(name.substr(descriptor.name.size())),
            };
        }
    }
    return {CommandKind::unknown, argument};
}

std::string command_names() {
    std::string result;
    for (const CommandDescriptor& descriptor : descriptors) {
        if (!result.empty()) result += ", ";
        result += descriptor.name;
        if (descriptor.form == CommandForm::handle_suffix) result += "Name";
    }
    return result;
}

} // namespace cha::web
