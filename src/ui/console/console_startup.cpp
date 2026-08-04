#include "ui/console/console_startup.h"

#include <string_view>
#include <utility>

namespace cha {
namespace {

ArgumentError argument_error(std::string message) {
    return {std::move(message), 2};
}

} // namespace

std::variant<ConsoleOptions, ArgumentError> parse_console_arguments(
    int argc,
    const char* const* argv) {
    ConsoleOptions options;
    bool color_selected{};
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--check") {
            options.check = true;
        } else if (argument.starts_with("--color=")) {
            color_selected = true;
            const std::string_view value =
                argument.substr(std::string_view("--color=").size());
            if (value == "always") {
                options.color = ColorMode::always;
            } else if (value == "never") {
                options.color = ColorMode::never;
            } else if (value == "auto") {
                options.color = ColorMode::automatic;
            } else {
                return argument_error(
                    "Unknown color mode '" + std::string(value) + "'");
            }
        } else {
            return argument_error(
                "Unknown option or argument '" + std::string(argument) + "'");
        }
    }

    if (options.check && color_selected) {
        return argument_error("--check cannot be used with --color");
    }

    return options;
}

} // namespace cha
