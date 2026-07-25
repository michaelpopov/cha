#include "ui/text/text_input.h"

#include "ui/text/command.h"
#include "ui/text/mention.h"

#include <utility>

namespace cha {

SessionUpdate handle_text_input(
    SessionController& controller,
    std::string input) {
    SessionUpdate update;
    if (input.empty()) {
        return update;
    }
    const Command command = parse_command(input);
    if (controller.generation_status().active) {
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
    if (!command.argument.empty() && command.kind != CommandKind::unknown) {
        update.clear_input = true;
        update.notice = "Command does not accept arguments";
        return update;
    }
    switch (command.kind) {
    case CommandKind::clear:
        return controller.clear_conversation();
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
            "Unknown command. Commands: /clear, /info, /agents, /@Name, /stop, /exit";
        return update;
    case CommandKind::text:
        return update;
    }
    return update;
}

} // namespace cha
