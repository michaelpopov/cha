#include "web/text_multicast.h"

#include "util/text.h"

namespace cha::web {
namespace {

std::size_t skip_space(std::string_view input, std::size_t index) {
    while (index < input.size() && is_space(input[index])) {
        ++index;
    }
    return index;
}

MulticastParseResult empty_prompt() {
    return MulticastParseError::empty_prompt;
}

} // namespace

MulticastParseResult parse_multicast_input(std::string_view argument) {
    argument = trim_view(argument);
    if (argument.empty()) {
        return empty_prompt();
    }
    if (!argument.starts_with('@')) {
        return MulticastInput{{}, std::string(argument)};
    }

    MulticastInput result;
    std::size_t index = 0;
    while (true) {
        if (index >= argument.size()) {
            return empty_prompt();
        }
        if (index + 1 < argument.size() && argument[index + 1] == '@') {
            const std::string_view text = trim_view(argument.substr(index + 1));
            return text.empty()
                ? empty_prompt()
                : MulticastInput{std::move(result.handles), std::string(text)};
        }

        const std::size_t handle_start = ++index;
        while (index < argument.size()
               && !is_space(argument[index])
               && argument[index] != ','
               && argument[index] != '.') {
            ++index;
        }
        if (index == handle_start) {
            return MulticastParseError::empty_handle;
        }
        result.handles.emplace_back(
            argument.substr(handle_start, index - handle_start));

        if (index == argument.size()) {
            return empty_prompt();
        }
        if (argument[index] == '.') {
            const std::string_view text = trim_view(argument.substr(index + 1));
            return text.empty()
                ? empty_prompt()
                : MulticastInput{std::move(result.handles), std::string(text)};
        }
        index = skip_space(argument, index);
        if (index >= argument.size()) {
            return empty_prompt();
        }
        if (argument[index] == ',') {
            index = skip_space(argument, index + 1);
            if (index >= argument.size() || argument[index] == ',') {
                return MulticastParseError::unexpected_comma;
            }
            if (argument[index] != '@') {
                return MulticastParseError::missing_separator;
            }
            continue;
        }
        if (argument[index] != '@') {
            return MulticastInput{
                std::move(result.handles),
                std::string(argument.substr(index)),
            };
        }
    }
}

std::string_view multicast_parse_error_message(MulticastParseError error) {
    switch (error) {
    case MulticastParseError::empty_prompt:
        return "Multicast prompt is empty";
    case MulticastParseError::empty_handle:
        return "Multicast recipient is empty";
    case MulticastParseError::unexpected_comma:
        return "Malformed multicast recipient list: unexpected comma";
    case MulticastParseError::missing_separator:
        return "Malformed multicast recipient list";
    }
    return "Malformed multicast command";
}

} // namespace cha::web
