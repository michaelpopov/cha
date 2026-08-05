#include "ui/text/application_dispatcher.h"

#include "application/chat_application.h"
#include "ui/text/application_command.h"
#include "ui/text/text_input.h"

#include <variant>

namespace cha {
ApplicationResult ApplicationDispatcher::handle(std::string input) {
    const auto parsed = parse_application_command(input);
    if (!parsed) {
        TextInputResult text = handle_text_input(
            application_.controller(), application_.selected_author_key(), std::move(input));
        return {.input_consumed = text.clear_input,
                .exit_requested = text.exit_requested,
                .session = std::move(text.session)};
    }
    if (const auto* parse_error = std::get_if<ApplicationCommandParseError>(&*parsed)) {
        return {.input_consumed = true,
                .notice = std::string(application_command_parse_error_message(*parse_error))};
    }

    const ApplicationCommand& command = std::get<ApplicationCommand>(*parsed);
    if (command.kind == ApplicationCommandKind::help) {
        std::vector<std::string> rows;
        rows.reserve(command_descriptors().size());
        for (const CommandDescriptor& descriptor : command_descriptors()) {
            rows.push_back(std::string(descriptor.syntax) + " — "
                + std::string(descriptor.description));
        }
        return {.input_consumed = true,
                .list = ApplicationList{"Commands", std::move(rows)}};
    }

    switch (command.kind) {
    case ApplicationCommandKind::iam: return application_.iam(command.names[0]);
    case ApplicationCommandKind::open: return application_.open(command.names[0], command.names[1]);
    case ApplicationCommandKind::create: return application_.create(command.names[0], command.names[1]);
    case ApplicationCommandKind::forums: return application_.forums();
    case ApplicationCommandKind::sessions: return application_.sessions(command.names[0]);
    case ApplicationCommandKind::members: return application_.members(command.names[0]);
    case ApplicationCommandKind::personas: return application_.personas();
    case ApplicationCommandKind::help: break;
    }
    return {};
}
} // namespace cha
