#include "command.h"

namespace cha {
namespace {

std::string trim(std::string_view value) {
    const std::size_t first = value.find_first_not_of(" \t");
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = value.find_last_not_of(" \t");
    return std::string(value.substr(first, last - first + 1));
}

} // namespace

Command parse_command(std::string_view input) {
    if (!input.starts_with('/')) {
        return {};
    }

    const std::size_t separator = input.find_first_of(" \t");
    const std::string_view name = input.substr(0, separator);
    const std::string argument = separator == std::string_view::npos ? "" : trim(input.substr(separator));

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
    return {CommandKind::unknown, argument};
}

} // namespace cha
