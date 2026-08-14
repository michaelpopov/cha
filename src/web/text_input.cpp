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
    const Command command = parse_command(input);
    if (controller.is_generating()) {
        if (command.kind == CommandKind::stop && command.argument.empty()) {
            result.session = controller.request_stop();
            result.clear_input = true;
            return result;
        }
        result.session.notice = std::string(generation_in_progress_notice);
        return result;
    }
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
    if (command.kind == CommandKind::session_provider) {
        result.clear_input = true;
        result.session = controller.set_session_provider(command.argument);
        return result;
    }
    if (command.kind == CommandKind::session_style) {
        result.clear_input = true;
        result.session = controller.set_session_style(command.argument);
        return result;
    }
    if (!command.argument.empty() && command.kind != CommandKind::unknown) {
        result.clear_input = true;
        result.session.notice = "Command does not accept arguments";
        return result;
    }
    switch (command.kind) {
    case CommandKind::clear:
        result.session = controller.clear_transcript(); break;
    case CommandKind::hide_on:
        result.session = controller.open_offrecord(); break;
    case CommandKind::hide:
        result.session = controller.extend_offrecord(); break;
    case CommandKind::hide_off:
        result.session = controller.restore_offrecord(); break;
    case CommandKind::mcast:
        return result;
    case CommandKind::session_provider:
        return result;
    case CommandKind::session_style:
        return result;
    case CommandKind::info:
        result.session = controller.session_information(); break;
    case CommandKind::stop:
        result.session = controller.request_stop();
        result.clear_input = true;
        return result;
    case CommandKind::exit:
        result.clear_input = true;
        result.close_session = true;
        return result;
    case CommandKind::characters:
        result.session = controller.character_information(); break;
    case CommandKind::set_default:
        result.session = controller.set_default_character(command.handle);
        if (requires_snapshot(result.session)) {
            result.persist_default_character_id =
                std::string(controller.view().default_character_id);
        }
        break;
    case CommandKind::set_persona:
        result.session = controller.set_default_persona(command.handle);
        if (requires_snapshot(result.session)) {
            result.persist_default_persona_id =
                std::string(controller.view().default_persona_id);
        }
        break;
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
