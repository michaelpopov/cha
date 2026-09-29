#include "daemon/chaweb_adapter.h"

#include "util/logging.h"
#include "util/path_name.h"
#include "util/text.h"

#include <optional>
#include <string>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

namespace cha::daemon {
namespace {

constexpr std::string_view json_type = "application/json";
constexpr std::string_view base_path = "/api/cha/v1";
constexpr std::string_view base_prefix = "/api/cha/v1/";
constexpr std::string_view rejected_input = "The input was rejected.";
constexpr std::string_view generic_failure =
    "The application operation failed";

std::string_view safe_message(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::not_found:
        return "That forum or session was not found.";
    case ErrorCode::invalid_argument:
        return "The request was not valid.";
    case ErrorCode::prompt_too_large:
        return "Prompt is too large.";
    case ErrorCode::command_timeout:
        return "The command outcome is unknown.";
    case ErrorCode::server_stopping:
        return "The server is shutting down.";
    case ErrorCode::application_unavailable:
        return "The application is unavailable.";
    default:
        return "The request could not be completed.";
    }
}

int status_for(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::not_found:
        return 404;
    case ErrorCode::invalid_argument:
    case ErrorCode::prompt_too_large:
        return 400;
    case ErrorCode::server_stopping:
    case ErrorCode::application_unavailable:
        return 503;
    default:
        return 500;
    }
}

Error error_body(ErrorCode code, std::string message = {}) {
    if (message.empty() || message == generic_failure) {
        message = std::string(safe_message(code));
    }
    return {code, std::move(message)};
}

Error from_application(const app::ApplicationError& error) {
    return error_body(error.code, error.what());
}

bool write_json(
    int fd,
    int status,
    const nlohmann::json& body,
    const std::atomic<bool>& stop) {
    return write_cgi(fd, status, json_type, body.dump(), stop);
}

bool write_error(
    int fd, int status, const Error& error, const std::atomic<bool>& stop) {
    return write_json(fd, status, error, stop);
}

bool write_code(
    int fd,
    ErrorCode code,
    const std::atomic<bool>& stop,
    std::string message = {}) {
    return write_error(fd, status_for(code), error_body(code, std::move(message)), stop);
}

bool is_json_content_type(std::string_view value) {
    const std::string_view trimmed = trim_view(value);
    const std::size_t cut = trimmed.find(';');
    const std::string_view media = trim_view(
        cut == std::string_view::npos ? trimmed : trimmed.substr(0, cut));
    return ascii_iequals(media, json_type);
}

bool require_json_post(
    const ScgiRequest& request, int fd, const std::atomic<bool>& stop) {
    if (is_json_content_type(request.content_type)) return true;
    write_error(
        fd,
        415,
        error_body(
            ErrorCode::invalid_argument,
            "The request must use application/json."),
        stop);
    return false;
}

std::optional<nlohmann::json> parse_object(
    std::string_view body, int fd, const std::atomic<bool>& stop) {
    nlohmann::json value;
    try {
        value = nlohmann::json::parse(body);
    } catch (const nlohmann::json::parse_error&) {
        write_code(fd, ErrorCode::invalid_argument, stop);
        return std::nullopt;
    }
    if (!value.is_object()) {
        write_code(fd, ErrorCode::invalid_argument, stop);
        return std::nullopt;
    }
    return value;
}

std::optional<std::string> parse_text_object(
    std::string_view body, int fd, const std::atomic<bool>& stop) {
    const auto object = parse_object(body, fd, stop);
    if (!object) return std::nullopt;
    if (object->size() != 1 || !object->contains("text")
        || !(*object)["text"].is_string()) {
        write_code(fd, ErrorCode::invalid_argument, stop);
        return std::nullopt;
    }
    return (*object)["text"].get<std::string>();
}

bool parse_empty_object(
    std::string_view body, int fd, const std::atomic<bool>& stop) {
    const auto object = parse_object(body, fd, stop);
    if (!object) return false;
    if (!object->empty()) {
        write_code(fd, ErrorCode::invalid_argument, stop);
        return false;
    }
    return true;
}

bool prompt_allowed(
    app::Application& application,
    const std::string& text,
    int fd,
    const std::atomic<bool>& stop) {
    if (text.size() <= application.settings().prompt_limit) return true;
    write_code(fd, ErrorCode::prompt_too_large, stop);
    return false;
}

enum class Route {
    unknown,
    bootstrap,
    sessions,
    session,
    input,
    stop,
};

struct ParsedRoute {
    Route route{Route::unknown};
    std::string forum_id;
    std::string session_id;
};

bool take_segment(std::string_view& rest, std::string_view& part) {
    if (rest.empty()) return false;
    const std::size_t cut = rest.find('/');
    if (cut == 0) return false;
    if (cut == std::string_view::npos) {
        part = rest;
        rest = {};
        return true;
    }
    part = rest.substr(0, cut);
    rest.remove_prefix(cut + 1);
    return true;
}

ParsedRoute parse_route(std::string_view uri) {
    if (uri == base_path || uri.ends_with('/')) return {};
    if (!uri.starts_with(base_prefix)) return {};
    std::string_view rest = uri.substr(base_prefix.size());
    std::string_view first;
    if (!take_segment(rest, first)) return {};
    if (first == "bootstrap") {
        if (!rest.empty()) return {};
        return {.route = Route::bootstrap};
    }
    if (first != "forums") return {};
    std::string_view forum;
    std::string_view sessions;
    if (!take_segment(rest, forum) || !take_segment(rest, sessions)
        || sessions != "sessions" || !is_url_safe_identifier(forum)) {
        return {};
    }
    if (rest.empty()) {
        return {.route = Route::sessions, .forum_id = std::string(forum)};
    }
    std::string_view session;
    if (!take_segment(rest, session) || !is_url_safe_identifier(session)) {
        return {};
    }
    ParsedRoute parsed{
        .route = Route::session,
        .forum_id = std::string(forum),
        .session_id = std::string(session),
    };
    if (rest.empty()) return parsed;
    std::string_view action;
    if (!take_segment(rest, action) || !rest.empty()) return {};
    if (action == "input") parsed.route = Route::input;
    else if (action == "stop") parsed.route = Route::stop;
    else return {};
    return parsed;
}

bool application_running(const app::ApplicationBootstrap& boot) {
    return boot.state == app::ApplicationState::running;
}

std::optional<ErrorCode> open_named(
    app::Application& application,
    std::string_view forum_id,
    std::string_view session_id,
    std::uint64_t epoch) {
    const auto opened =
        application.open_session(forum_id, session_id, epoch);
    if (const auto* code = std::get_if<ErrorCode>(&opened)) return *code;
    return std::nullopt;
}

bool cleanup_created(
    app::Application& application,
    std::string_view forum_id,
    std::string_view session_id,
    std::uint64_t epoch,
    ChaWebDeleteSession delete_session) {
    std::optional<ErrorCode> error;
    if (delete_session != nullptr) {
        error = delete_session(application, forum_id, session_id, epoch);
    } else {
        error = application.delete_session(forum_id, session_id, epoch);
    }
    if (!error || *error == ErrorCode::not_found) return true;
    log_warn("Could not delete rejected session " + std::string(session_id));
    return false;
}

bool write_cleanup_failure(int fd, const std::atomic<bool>& stop) {
    return write_code(fd, ErrorCode::internal_error, stop);
}

enum class InputOutcome { accepted, rejected, unknown, failed };

struct SubmitView {
    InputOutcome outcome{InputOutcome::failed};
    ErrorCode code{ErrorCode::internal_error};
    std::string message;
};

SubmitView classify_submit(const CommandSubmitResult& result) {
    if (const auto* done = std::get_if<CommandResult>(&result)) {
        if (done->session.input_consumed) {
            return {.outcome = InputOutcome::accepted};
        }
        const auto& notice = done->session.notice;
        return {
            .outcome = InputOutcome::rejected,
            .code = ErrorCode::invalid_argument,
            .message = notice && !notice->empty() ? *notice
                                                  : std::string(rejected_input),
        };
    }
    if (const auto* failure = std::get_if<CommandFailure>(&result)) {
        return {
            .outcome = failure->code == ErrorCode::invalid_argument
                ? InputOutcome::rejected
                : InputOutcome::failed,
            .code = failure->code,
            .message = failure->message.empty()
                ? std::string(safe_message(failure->code))
                : failure->message,
        };
    }
    if (const auto* code = std::get_if<ErrorCode>(&result)) {
        return {
            .outcome = *code == ErrorCode::command_timeout
                ? InputOutcome::unknown
                : InputOutcome::failed,
            .code = *code,
        };
    }
    return {};
}

bool write_rejected(
    int fd, const SubmitView& view, const std::atomic<bool>& stop) {
    return write_error(
        fd,
        422,
        error_body(ErrorCode::invalid_argument, view.message),
        stop);
}

bool write_failed(
    int fd, const SubmitView& view, const std::atomic<bool>& stop) {
    return write_code(fd, view.code, stop, view.message);
}

void serve_bootstrap(
    app::Application& application, int fd, const std::atomic<bool>& stop) {
    const app::ApplicationBootstrap boot = application.bootstrap();
    if (!application_running(boot)) {
        write_code(fd, ErrorCode::application_unavailable, stop);
        return;
    }
    write_json(fd, 200, boot.presentation, stop);
}

void serve_list(
    app::Application& application,
    const ParsedRoute& route,
    int fd,
    const std::atomic<bool>& stop) {
    const std::uint64_t epoch = application.context_epoch();
    const auto listing = application.list_sessions(route.forum_id, epoch);
    write_json(fd, 200, listing, stop);
}

void serve_snapshot(
    app::Application& application,
    const ParsedRoute& route,
    int fd,
    const std::atomic<bool>& stop) {
    const std::uint64_t epoch = application.context_epoch();
    if (const auto error = open_named(
            application, route.forum_id, route.session_id, epoch)) {
        write_code(fd, *error, stop);
        return;
    }
    const CommandSubmitResult result =
        application.snapshot(route.forum_id, route.session_id, epoch);
    if (const auto* snapshot = std::get_if<SessionSnapshot>(&result)) {
        write_json(fd, 200, *snapshot, stop);
        return;
    }
    write_failed(fd, classify_submit(result), stop);
}

void serve_input(
    app::Application& application,
    const ScgiRequest& request,
    const ParsedRoute& route,
    int fd,
    const std::atomic<bool>& stop) {
    if (!require_json_post(request, fd, stop)) return;
    const auto text = parse_text_object(request.body, fd, stop);
    if (!text) return;
    if (!prompt_allowed(application, *text, fd, stop)) return;
    const std::uint64_t epoch = application.context_epoch();
    if (const auto error = open_named(
            application, route.forum_id, route.session_id, epoch)) {
        write_code(fd, *error, stop);
        return;
    }
    const SubmitView view = classify_submit(application.submit(
        route.forum_id, route.session_id, RawCommand{*text}, epoch));
    if (view.outcome == InputOutcome::accepted) {
        write_cgi(fd, 204, {}, {}, stop);
        return;
    }
    if (view.outcome == InputOutcome::rejected) {
        write_rejected(fd, view, stop);
        return;
    }
    write_failed(fd, view, stop);
}

void serve_stop(
    app::Application& application,
    const ScgiRequest& request,
    const ParsedRoute& route,
    int fd,
    const std::atomic<bool>& stop) {
    if (!require_json_post(request, fd, stop)) return;
    if (!parse_empty_object(request.body, fd, stop)) return;
    const std::uint64_t epoch = application.context_epoch();
    if (const auto error = open_named(
            application, route.forum_id, route.session_id, epoch)) {
        write_code(fd, *error, stop);
        return;
    }
    const CommandSubmitResult result =
        application.stop(route.forum_id, route.session_id, epoch);
    if (std::holds_alternative<CommandResult>(result)) {
        write_cgi(fd, 204, {}, {}, stop);
        return;
    }
    write_failed(fd, classify_submit(result), stop);
}

void serve_create(
    app::Application& application,
    const ScgiRequest& request,
    const ParsedRoute& route,
    int fd,
    const std::atomic<bool>& stop,
    ChaWebDeleteSession delete_session) {
    if (!require_json_post(request, fd, stop)) return;
    const auto text = parse_text_object(request.body, fd, stop);
    if (!text) return;
    if (!prompt_allowed(application, *text, fd, stop)) return;
    const std::uint64_t epoch = application.context_epoch();
    (void)application.get_forum(route.forum_id, epoch);

    std::string session_id;
    auto fail_created = [&](int status, const Error& error) {
        if (!cleanup_created(
                application,
                route.forum_id,
                session_id,
                epoch,
                delete_session)) {
            write_cleanup_failure(fd, stop);
            return;
        }
        write_error(fd, status, error, stop);
    };

    try {
        const CreateSessionSuccess created =
            application.create_session(route.forum_id, "", epoch);
        session_id = created.id;
        if (const auto error =
                open_named(application, route.forum_id, session_id, epoch)) {
            fail_created(status_for(*error), error_body(*error));
            return;
        }
        const SubmitView view = classify_submit(application.submit(
            route.forum_id, session_id, RawCommand{*text}, epoch));
        if (view.outcome == InputOutcome::accepted) {
            write_json(fd, 201, created, stop);
            return;
        }
        if (view.outcome == InputOutcome::unknown) {
            write_failed(fd, view, stop);
            return;
        }
        if (view.outcome == InputOutcome::rejected) {
            fail_created(
                422,
                error_body(ErrorCode::invalid_argument, view.message));
            return;
        }
        fail_created(status_for(view.code), error_body(view.code, view.message));
    } catch (const app::ApplicationError& error) {
        if (session_id.empty()) throw;
        fail_created(status_for(error.code), from_application(error));
    } catch (const std::exception&) {
        if (!session_id.empty()
            && !cleanup_created(
                application,
                route.forum_id,
                session_id,
                epoch,
                delete_session)) {
            write_cleanup_failure(fd, stop);
            return;
        }
        throw;
    }
}

} // namespace

bool is_chaweb_request(std::string_view document_uri) noexcept {
    return document_uri == base_path || document_uri.starts_with(base_prefix);
}

void handle_chaweb_request(
    app::Application& application,
    const ScgiRequest& request,
    int fd,
    std::atomic<bool>& stop,
    ChaWebDeleteSession delete_session) {
    try {
        if (stop.load()) return;
        const ParsedRoute route = parse_route(request.document_uri);
        if (request.method == "GET" && route.route == Route::bootstrap) {
            serve_bootstrap(application, fd, stop);
            return;
        }
        if (request.method == "GET" && route.route == Route::sessions) {
            serve_list(application, route, fd, stop);
            return;
        }
        if (request.method == "POST" && route.route == Route::sessions) {
            serve_create(
                application, request, route, fd, stop, delete_session);
            return;
        }
        if (request.method == "GET" && route.route == Route::session) {
            serve_snapshot(application, route, fd, stop);
            return;
        }
        if (request.method == "POST" && route.route == Route::input) {
            serve_input(application, request, route, fd, stop);
            return;
        }
        if (request.method == "POST" && route.route == Route::stop) {
            serve_stop(application, request, route, fd, stop);
            return;
        }
        write_code(fd, ErrorCode::not_found, stop, "The request was not found.");
    } catch (const app::ApplicationError& error) {
        write_error(fd, status_for(error.code), from_application(error), stop);
    } catch (const std::exception& error) {
        log_error(error.what());
        write_code(fd, ErrorCode::internal_error, stop);
    }
}

} // namespace cha::daemon
