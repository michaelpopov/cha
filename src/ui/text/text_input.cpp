#include "ui/text/text_input.h"

#include "ui/text/command.h"
#include "ui/text/mcast.h"
#include "ui/text/mention.h"

#include <utility>

namespace cha {
namespace {

SessionUpdate handle_multicast_input(
    SessionController& controller,
    std::string_view argument) {
    SessionUpdate update{.clear_input = true};
    MulticastParseResult parsed = parse_multicast_input(argument);
    if (const auto* error = std::get_if<MulticastParseError>(&parsed)) {
        update.notice = std::string(multicast_parse_error_message(*error));
        return update;
    }

    MulticastInput input = std::get<MulticastInput>(std::move(parsed));
    SessionUpdate started = controller.start_multicast(
        std::move(input.text), std::move(input.handles));
    started.clear_input = started.clear_input || update.clear_input;
    return started;
}

} // namespace

SessionUpdate handle_text_input(
    SessionController& controller,
    std::string input) {
    SessionUpdate update;
    if (input.empty()) {
        return update;
    }
    const Command command = parse_command(input);
    if (controller.is_generating()) {
        if (command.kind == CommandKind::stop && command.argument.empty()) {
            update = controller.request_stop();
            update.clear_input = true;
            return update;
        }
        update.notice = std::string(generation_in_progress_notice);
        return update;
    }
    if (command.kind == CommandKind::text) {
        AddressedPrompt prompt = parse_addressed_prompt(input);
        return controller.submit_prompt(
            std::move(prompt.text),
            std::move(prompt.handle));
    }
    if (command.kind == CommandKind::mcast) {
        return handle_multicast_input(controller, command.argument);
    }
    if (!command.argument.empty() && command.kind != CommandKind::unknown) {
        update.clear_input = true;
        update.notice = "Command does not accept arguments";
        return update;
    }
    switch (command.kind) {
    case CommandKind::clear:
        return controller.clear_transcript();
    case CommandKind::hide_on:
        return controller.open_offrecord();
    case CommandKind::hide:
        return controller.extend_offrecord();
    case CommandKind::hide_off:
        return controller.restore_offrecord();
    case CommandKind::mcast:
        return update;
    case CommandKind::info:
        return controller.session_information();
    case CommandKind::stop:
        update = controller.request_stop();
        update.clear_input = true;
        return update;
    case CommandKind::exit:
        update.clear_input = true;
        update.end_session = true;
        return update;
    case CommandKind::agents:
        return controller.agent_information();
    case CommandKind::set_default:
        return controller.set_default_agent(command.handle);
    case CommandKind::unknown:
        update.clear_input = true;
        update.notice =
            "Unknown command. Commands: /clear, /hide-on, /hide, /hide-off, /mcast, /info, /agents, /@Name, /stop, /exit";
        return update;
    case CommandKind::text:
        return update;
    }
    return update;
}

} // namespace cha
