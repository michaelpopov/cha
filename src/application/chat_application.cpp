#include "application/chat_application.h"

#include "application/builtins.h"
#include "session/generation_status.h"
#include "session/session_controller.h"
#include "util/text.h"

#include <exception>
#include <utility>

namespace cha {
ChatApplication::ChatApplication(const Workspace& workspace, WakeNotifier& notifier)
    : environment_(workspace), notifier_(notifier), current_(environment_.open_welcome(notifier_)) {}

ChatApplication::~ChatApplication() {
    if (current_.controller) {
        try { current_.controller->shutdown(); } catch (...) { }
    }
}

SessionController& ChatApplication::controller() noexcept { return *current_.controller; }
const SessionDescriptor& ChatApplication::descriptor() const noexcept { return current_.descriptor; }

ApplicationResult ChatApplication::error(std::string message, bool consumed) const {
    return {.input_consumed = consumed, .notice = std::move(message)};
}

ApplicationResult ChatApplication::iam(std::string_view name) {
    if (current_.controller->is_generating()) return error(std::string(generation_in_progress_notice), false);
    const Persona* persona = environment_.personas().find(name);
    if (persona == nullptr) return error("Unknown persona '" + std::string(name) + "'");
    selected_persona_ = persona->display_name;
    selected_author_key_ = persona->id;
    return {.input_consumed = true, .persona_changed = true,
            .notice = "Now speaking as " + selected_persona_};
}

ApplicationResult ChatApplication::forums() const {
    if (current_.controller->is_generating()) return error(std::string(generation_in_progress_notice), false);
    auto rows = environment_.forums().custom_names();
    if (rows.empty()) return {.input_consumed = true, .notice = "No workspace forums"};
    return {.input_consumed = true, .list = ApplicationList{"Forums", std::move(rows)}};
}

ApplicationResult ChatApplication::personas() const {
    if (current_.controller->is_generating()) return error(std::string(generation_in_progress_notice), false);
    auto rows = environment_.personas().custom_names();
    if (rows.empty()) return {.input_consumed = true, .notice = "No workspace personas"};
    return {.input_consumed = true, .list = ApplicationList{"Personas", std::move(rows)}};
}

ApplicationResult ChatApplication::sessions(std::string_view forum) {
    if (current_.controller->is_generating()) return error(std::string(generation_in_progress_notice), false);
    try {
        const Forum* resolved = environment_.forums().find(forum);
        if (resolved == nullptr) return error("Unknown forum '" + std::string(forum) + "'");
        SessionSource& source = environment_.forums().source_for(forum);
        std::vector<std::string> rows;
        for (const SessionSummary& item : source.list()) {
            std::string row = item.label;
            if (!item.error.empty()) row += " [corrupt]";
            if (item.ambiguous) row += " [ambiguous]";
            rows.push_back(std::move(row));
        }
        if (rows.empty()) return {.input_consumed = true, .notice = "No stored sessions in forum '" + resolved->display_name + "'"};
        return {.input_consumed = true, .list = ApplicationList{"Sessions in " + resolved->display_name, std::move(rows)}};
    } catch (const std::exception& exception) { return error(exception.what()); }
}

ApplicationResult ChatApplication::switch_to(OpenedSession prepared, bool consumed) {
    current_.controller->shutdown();
    current_.controller.reset();
    current_ = std::move(prepared);
    return {.input_consumed = consumed, .session_changed = true,
            .notice = "Opened session '" + current_.descriptor.session_label + "' in forum '" + current_.descriptor.forum_display_name + "'",
            .descriptor = current_.descriptor};
}

ApplicationResult ChatApplication::open(std::string_view forum, std::string_view session) {
    if (current_.controller->is_generating()) return error(std::string(generation_in_progress_notice), false);
    const Forum* resolved = environment_.forums().find(forum);
    if (resolved == nullptr) return error("Unknown forum '" + std::string(forum) + "'");
    if (fold_ascii(current_.descriptor.forum_display_name) == fold_ascii(resolved->display_name)
        && fold_ascii(current_.descriptor.session_label) == fold_ascii(session)) {
        return {.input_consumed = true, .notice = "Session '" + current_.descriptor.session_label + "' is already open"};
    }
    OpenedSession prepared;
    try {
        prepared = resolved == &builtin_entrance()
            && fold_ascii(session) == fold_ascii(welcome_name)
            ? environment_.open_welcome(notifier_)
            : environment_.forums().source_for(forum).open(session, notifier_);
    } catch (const std::exception& exception) { return error(exception.what()); }
    return switch_to(std::move(prepared));
}

ApplicationResult ChatApplication::create(std::string forum, std::string session) {
    if (current_.controller->is_generating()) return error(std::string(generation_in_progress_notice), false);
    const Forum* resolved = environment_.forums().find(forum);
    if (resolved == nullptr) return error("Unknown forum '" + forum + "'");
    const std::string public_forum = resolved->display_name;
    const std::string public_session = session;
    OpenedSession prepared;
    try { prepared = environment_.forums().source_for(forum).create(std::move(session), notifier_); }
    catch (const SessionCreatedOpenError& exception) {
        return error("Session '" + public_session + "' was created in forum '" + public_forum
            + "' but could not be opened: " + exception.what());
    }
    catch (const std::exception& exception) { return error(exception.what()); }
    return switch_to(std::move(prepared));
}
} // namespace cha
