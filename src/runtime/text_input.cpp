#include "runtime/text_input.h"

#include "session/session_controller.h"
#include "util/text.h"

#include <array>
#include <utility>

namespace cha {
namespace {

struct CommandDescriptor {
    std::string_view name;
    CommandKind kind;
};

constexpr std::array descriptors{
    CommandDescriptor{"/mcast", CommandKind::mcast},
};

std::size_t skip_space(std::string_view input, std::size_t index) {
    while (index < input.size() && is_space(input[index])) {
        ++index;
    }
    return index;
}

MulticastParseResult empty_prompt() {
    return MulticastParseError::empty_prompt;
}

CommandResult handle_multicast_input(
    SessionController& controller,
    std::string_view author_id,
    std::string_view argument) {
    CommandResult result{.clear_input = true};
    MulticastParseResult parsed = parse_multicast_input(argument);
    if (const auto* error = std::get_if<MulticastParseError>(&parsed)) {
        result.session.notice = std::string(multicast_parse_error_message(*error));
        return result;
    }

    MulticastInput input = std::get<MulticastInput>(std::move(parsed));
    result.session = controller.start_multicast(
        author_id,
        std::move(input.text), std::move(input.handles));
    result.clear_input = result.clear_input || result.session.input_consumed;
    return result;
}

} // namespace

Command parse_command(std::string_view input) {
    if (!input.starts_with('/')) {
        return {};
    }

    const std::size_t separator = find_whitespace(input);
    const std::string_view name = input.substr(0, separator);
    const std::string argument = separator == std::string_view::npos
        ? ""
        : std::string(trim_view(input.substr(separator)));

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

AddressedPrompt parse_addressed_prompt(std::string_view input) {
    std::size_t start = 0;
    while (start < input.size() && is_space(input[start])) {
        ++start;
    }
    const std::string_view trimmed = input.substr(start);
    if (!trimmed.starts_with('@')) {
        return {{}, std::string(input)};
    }
    if (trimmed.starts_with("@@")) {
        std::string text(input);
        text.erase(start, 1);
        return {{}, std::move(text)};
    }
    std::size_t end = 1;
    while (end < trimmed.size() && !is_space(trimmed[end])) {
        ++end;
    }
    if (end == 1) {
        return {{}, std::string(input)};
    }
    std::size_t body = end;
    while (body < trimmed.size() && is_space(trimmed[body])) {
        ++body;
    }
    return {
        std::string(trimmed.substr(1, end - 1)),
        std::string(trimmed.substr(body)),
    };
}

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

CommandResult handle_text_input(
    SessionController& controller,
    std::string_view author_id,
    std::string input,
    std::shared_ptr<SubmissionState> submission) {
    CommandResult result;
    if (trim_view(input).empty()) {
        return result;
    }
    if (controller.is_generating()) {
        result.session.notice = std::string(generation_in_progress_notice);
        return result;
    }
    const Command command = parse_command(input);
    if (command.kind == CommandKind::text) {
        AddressedPrompt prompt = parse_addressed_prompt(input);
        result.session = controller.submit_prompt(
            author_id,
            std::move(prompt.text),
            std::move(prompt.handle), std::move(submission));
        result.clear_input = result.session.input_consumed;
        return result;
    }
    if (command.kind == CommandKind::mcast) {
        return handle_multicast_input(controller, author_id, command.argument);
    }
    result.clear_input = true;
    result.session.notice = "Unknown command. Commands: " + command_names();
    return result;
}

} // namespace cha
