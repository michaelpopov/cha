#include "interfaces/text/text_input.h"

#include "interfaces/text/command.h"
#include "interfaces/text/mention.h"

#include <utility>

namespace cha {

CoordinatorUpdate handle_text_input(
    ChatCoordinator& coordinator,
    std::string input) {
    CoordinatorUpdate update;
    if (input.empty()) {
        return update;
    }
    const Command command = parse_command(input);
    if (coordinator.generation_status().active) {
        if (command.kind == CommandKind::stop && command.argument.empty()) {
            update = coordinator.request_stop();
            update.clear_input = true;
            return update;
        }
        update.notice = std::string(generation_in_progress_notice);
        return update;
    }
    if (command.kind == CommandKind::text) {
        AddressedPrompt prompt = parse_addressed_prompt(input);
        return coordinator.submit_prompt(
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
        return coordinator.clear_conversation();
    case CommandKind::info:
        return coordinator.session_information();
    case CommandKind::stop:
        update = coordinator.request_stop();
        update.clear_input = true;
        return update;
    case CommandKind::exit:
        update.clear_input = true;
        update.end_session = true;
        return update;
    case CommandKind::agents:
        return coordinator.agent_information();
    case CommandKind::set_default:
        return coordinator.set_default_agent(command.handle);
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
