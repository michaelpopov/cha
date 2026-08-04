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
        return {.input_consumed = true,
                .list = ApplicationList{"Commands",
                                        {"/iam <persona> — Change the current persona.",
                                         "/open <forum> <session> — Open a session.",
                                         "/create <forum> <session> — Create and open a session.",
                                         "/forums — List workspace forums.",
                                         "/sessions <forum> — List stored sessions.",
                                         "/personas — List workspace personas.",
                                         "/help — List commands.",
                                         "/clear — Clear the transcript.",
                                         "/hide-on — Begin an off-record span.",
                                         "/hide — Extend an off-record span.",
                                         "/hide-off — End an off-record span.",
                                         "/mcast <targets> <text> — Send a multicast prompt.",
                                         "/info — Show session information.",
                                         "/agents — List forum agents.",
                                         "/@Name — Set the default agent.",
                                         "/stop — Stop generation.",
                                         "/exit — Exit the application."}}};
    }

    switch (command.kind) {
    case ApplicationCommandKind::iam: return application_.iam(command.names[0]);
    case ApplicationCommandKind::open: return application_.open(command.names[0], command.names[1]);
    case ApplicationCommandKind::create: return application_.create(command.names[0], command.names[1]);
    case ApplicationCommandKind::forums: return application_.forums();
    case ApplicationCommandKind::sessions: return application_.sessions(command.names[0]);
    case ApplicationCommandKind::personas: return application_.personas();
    case ApplicationCommandKind::help: break;
    }
    return {};
}
} // namespace cha
