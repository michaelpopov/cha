#include "ui/console/console_startup.h"

#include "session/session_controller.h"
#include "ui/console/console_writer.h"

#include <algorithm>
#include <ostream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace cha {
namespace {

ArgumentError argument_error(std::string message) {
    return {std::move(message), 2};
}

std::string listing_field(std::string_view value) {
    std::string normalized(value);
    std::replace(normalized.begin(), normalized.end(), '\t', ' ');
    std::replace(normalized.begin(), normalized.end(), '\n', ' ');
    std::replace(normalized.begin(), normalized.end(), '\r', ' ');
    return sanitize_console_text(normalized);
}

} // namespace

std::variant<ConsoleOptions, ArgumentError> parse_console_arguments(
    int argc,
    const char* const* argv) {
    ConsoleOptions options;
    bool session_selected = false;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--list-forums") {
            options.list_forums = true;
        } else if (argument == "--list-sessions") {
            options.list_sessions = true;
        } else if (argument == "--check") {
            options.check_forum = true;
        } else if (argument == "--forum"
            || argument == "--session"
            || argument == "--new") {
            if (index + 1 >= argc) {
                return argument_error(
                    "Missing value for " + std::string(argument));
            }
            const std::string value(argv[++index]);
            if (argument == "--forum") {
                options.forum = value;
            } else if (argument == "--session") {
                session_selected = true;
                options.session_id = value;
            } else {
                options.new_label = value;
            }
        } else if (argument.starts_with("--color=")) {
            const std::string_view value =
                argument.substr(std::string_view("--color=").size());
            if (value == "always") {
                options.color = ColorMode::always;
            } else if (value == "never") {
                options.color = ColorMode::never;
            } else if (value == "auto") {
                options.color = ColorMode::automatic;
            } else {
                return argument_error(
                    "Unknown color mode '" + std::string(value) + "'");
            }
        } else {
            return argument_error(
                "Unknown option or argument '" + std::string(argument) + "'");
        }
    }

    if (options.list_forums) {
        return options;
    }
    if (options.forum.empty()) {
        return argument_error("--forum is required");
    }
    if (options.list_sessions && options.check_forum) {
        return argument_error(
            "--list-sessions and --check cannot be used together");
    }
    if (options.list_sessions) {
        return options;
    }
    // Validate selection only after listing modes have had their documented
    // precedence over otherwise irrelevant selection flags.
    if (session_selected && options.session_id.empty()) {
        return argument_error("--session requires a session ID");
    }
    if (session_selected && options.new_label) {
        return argument_error("--session and --new cannot be used together");
    }
    if (options.check_forum && (session_selected || options.new_label)) {
        return argument_error(
            "--check cannot be used with --session or --new");
    }
    if (options.check_forum) {
        return options;
    }
    if (!session_selected && !options.new_label) {
        options.new_label = "";
    }
    return options;
}

void write_forum_listing(const Workspace& workspace, std::ostream& out) {
    for (const std::string& forum : workspace.forums()) {
        out << listing_field(workspace.load_forum(forum).display_name) << '\n';
    }
}

void write_session_listing(
    const Workspace& workspace,
    const std::string& forum,
    std::ostream& out) {
    for (const SessionSummary& session : workspace.sessions(forum)) {
        out << listing_field(session.id) << '\t'
            << listing_field(session.label) << '\t'
            << listing_field(session.error) << '\n';
    }
}

void write_forum_check(
    const Workspace& workspace,
    const std::string& forum_name,
    std::ostream& out) {
    const Forum forum = workspace.check_forum(forum_name);
    out << "Forum '" << listing_field(forum.name) << "' is valid ("
        << forum.persona_names.size() << ' '
        << (forum.persona_names.size() == 1 ? "persona" : "personas")
        << ").\n";
}

ConsoleSelection open_console_session(
    const Workspace& workspace,
    const ConsoleOptions& options,
    WakeNotifier& notifier) {
    if (options.new_label) {
        CreatedSession created =
            workspace.create_session(
                options.forum,
                *options.new_label,
                notifier);
        return {
            .controller = std::move(created.controller),
            .session_id = std::move(created.id),
        };
    }

    const std::vector<SessionSummary> sessions =
        workspace.sessions(options.forum);
    const auto found = std::find_if(
        sessions.begin(),
        sessions.end(),
        [&options](const SessionSummary& session) {
            return session.id == options.session_id;
        });
    if (found == sessions.end()) {
        throw std::runtime_error(
            "Session '" + options.session_id + "' does not exist");
    }
    if (!found->error.empty()) {
        throw std::runtime_error(found->error);
    }
    return {
        .controller =
            workspace.open_session(
                options.forum,
                options.session_id,
                notifier),
        .session_id = options.session_id,
    };
}

} // namespace cha
