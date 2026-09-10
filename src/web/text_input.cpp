#include "web/text_input.h"

#include "session/session_controller.h"
#include "web/text_command.h"
#include "web/text_mention.h"
#include "web/text_multicast.h"

#include <utility>

namespace cha::web {
namespace {

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

CommandResult handle_text_input(
    SessionController& controller,
    std::string_view author_id,
    std::string input) {
    CommandResult result;
    if (input.empty()) {
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
            std::move(prompt.handle));
        result.clear_input = result.session.input_consumed;
        return result;
    }
    if (command.kind == CommandKind::mcast) {
        return handle_multicast_input(controller, author_id, command.argument);
    }
    if (!command.argument.empty() && command.kind != CommandKind::unknown) {
        result.clear_input = true;
        result.session.notice = "Command does not accept arguments";
        return result;
    }
    switch (command.kind) {
    case CommandKind::cover:
        result.session = controller.cover_conversation(); break;
    case CommandKind::uncover:
        result.session = controller.uncover_conversation(); break;
    case CommandKind::mcast:
        return result;
    case CommandKind::unknown:
        result.clear_input = true;
        result.session.notice = "Unknown command. Commands: " + command_names();
        return result;
    case CommandKind::text:
        return result;
    }
    result.clear_input = result.session.input_consumed;
    return result;
}

} // namespace cha::web
