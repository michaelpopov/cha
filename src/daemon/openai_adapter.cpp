#include "daemon/openai_adapter.h"

#include "util/logging.h"
#include "workspace/builtins.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <set>
#include <sstream>
#include <thread>
#include <utility>
#include <vector>

namespace cha::daemon {
namespace {

constexpr std::string_view models_path = "/v1/models";
constexpr std::string_view chat_path = "/v1/chat/completions";
constexpr std::string_view missing_session_message =
    "This chat has no CHA session. Start a new chat.";
constexpr auto poll_interval = std::chrono::milliseconds{25};
constexpr auto keepalive_interval = std::chrono::seconds{15};

std::chrono::steady_clock::time_point steady_now(const TurnClock& clock) {
    if (clock) return clock();
    return std::chrono::steady_clock::now();
}

std::string completion_id() {
    static std::atomic<std::uint64_t> next{1};
    std::ostringstream text;
    text << "chatcmpl-" << std::hex << next.fetch_add(1);
    return text.str();
}

std::int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

ChatParseError make_error(
    int status,
    std::string message,
    std::string type,
    std::string code) {
    return {
        .status = status,
        .message = std::move(message),
        .type = std::move(type),
        .code = std::move(code),
    };
}

bool write_json(
    int fd,
    int status,
    const nlohmann::json& body,
    std::atomic<bool>& stop) {
    return write_cgi(
        fd, status, "application/json", body.dump(), stop);
}

bool write_error(
    int fd,
    const ChatParseError& error,
    std::atomic<bool>& stop) {
    return write_json(
        fd,
        error.status,
        openai_error(error.message, error.type, error.code),
        stop);
}

bool model_is_exposed(
    app::Application& application, std::string_view model) {
    if (model == entrance_id) return false;
    for (const ForumSummary& forum :
         application.bootstrap().presentation.forums) {
        if (forum.id == entrance_id) continue;
        if (forum.id == model) return true;
    }
    return false;
}

std::string_view failure_text(ErrorCode code) {
    switch (code) {
    case ErrorCode::not_found:
        return "That forum or session was not found.";
    case ErrorCode::command_timeout:
        return "The command outcome is unknown.";
    case ErrorCode::session_not_live:
        return "That session is not open.";
    case ErrorCode::application_unavailable:
        return "The application is unavailable.";
    case ErrorCode::server_stopping:
        return "The server is shutting down.";
    default:
        return "The request could not be completed.";
    }
}

EntryId highest_entry_id(const SessionSnapshot& snapshot) {
    EntryId highest = 0;
    for (const TranscriptEntry& entry : snapshot.transcript) {
        highest = std::max(highest, entry.id);
    }
    return highest;
}

struct Reply {
    EntryId id{};
    EntryKind kind{EntryKind::notice};
    std::string display_name;
    std::string text;
    std::string emitted;
    bool stopped{false};
};

Reply* find_reply(std::vector<Reply>& replies, EntryId id) {
    for (Reply& reply : replies) {
        if (reply.id == id) return &reply;
    }
    return nullptr;
}

std::string render_replies(const std::vector<Reply>& replies) {
    std::string text;
    bool human = false;
    for (const Reply& reply : replies) {
        if (reply.kind == EntryKind::human) human = true;
        if (reply.kind != EntryKind::character || reply.text.empty()) continue;
        if (!text.empty()) text += "\n\n";
        text += "**";
        text += reply.display_name;
        text += ":** ";
        text += reply.text;
    }
    if (!text.empty()) return text;
    if (human) return "(recorded)";
    return {};
}

std::string provider_failure_message(const std::vector<Reply>& replies) {
    for (const Reply& reply : replies) {
        if (reply.kind == EntryKind::error && !reply.text.empty()) {
            return reply.text;
        }
    }
    return "The provider failed.";
}

std::chrono::steady_clock::time_point cleanup_deadline(
    app::Application& application, const DaemonShutdown& shutdown) {
    if (shutdown.armed) return shutdown.deadline;
    return std::chrono::steady_clock::now()
        + application.settings().command_deadline;
}

std::optional<ErrorCode> enqueue_cleanup_command(
    app::Application& application,
    std::string_view forum,
    std::string_view session_id,
    WebCommand command,
    std::uint64_t epoch,
    const DaemonShutdown& shutdown) {
    const auto queued = application.submit_async(
        forum,
        session_id,
        std::move(command),
        epoch,
        cleanup_deadline(application, shutdown));
    if (const auto* error = std::get_if<ErrorCode>(&queued)) return *error;
    return std::nullopt;
}

bool harmless_cleanup_error(ErrorCode code) {
    return code == ErrorCode::session_not_live
        || code == ErrorCode::session_stopping
        || code == ErrorCode::not_found
        || code == ErrorCode::server_stopping
        || code == ErrorCode::application_unavailable;
}

struct CreatedSessionCleanup {
    app::Application& application;
    std::string forum;
    std::string session_id;
    std::uint64_t epoch{};
    DaemonShutdown& shutdown;
    bool remove{false};

    ~CreatedSessionCleanup() {
        if (!remove || shutdown.requested()) return;
        try {
            const auto error = application.delete_session(
                forum, session_id, epoch, false);
            if (error && *error != ErrorCode::not_found) {
                shutdown.request_stop();
            }
        } catch (const std::exception& error) {
            log_error(error.what());
            shutdown.request_stop();
        }
    }
};

struct SubscriptionCleanup {
    app::Application& application;
    std::string forum;
    std::string session_id;
    std::string connection_id;
    std::string subscription_id;
    std::uint64_t epoch{};
    DaemonShutdown& shutdown;
    std::shared_ptr<LiveSession> session;
    bool subscribed{false};
    bool disarm_after_cleanup{false};
    int uncaught_on_entry{std::uncaught_exceptions()};

    SubscriptionCleanup(
        app::Application& application_in,
        std::string forum_in,
        std::string session_in,
        std::string connection_in,
        std::string subscription_in,
        std::uint64_t epoch_in,
        DaemonShutdown& shutdown_in)
        : application(application_in),
          forum(std::move(forum_in)),
          session_id(std::move(session_in)),
          connection_id(std::move(connection_in)),
          subscription_id(std::move(subscription_in)),
          epoch(epoch_in),
          shutdown(shutdown_in) {}

    ~SubscriptionCleanup() {
        if (std::uncaught_exceptions() > uncaught_on_entry) {
            shutdown.request_stop();
        }
        try {
            if (subscribed) {
                const auto error = enqueue_cleanup_command(
                    application,
                    forum,
                    session_id,
                    UnsubscribeCommand{
                        connection_id, epoch, subscription_id},
                    epoch,
                    shutdown);
                if (error && !harmless_cleanup_error(*error)) {
                    shutdown.request_stop();
                }
            }
        } catch (const std::exception& error) {
            log_error(error.what());
            shutdown.request_stop();
        }
        subscribed = false;
        session.reset();
        if (disarm_after_cleanup) shutdown.disarm_if_idle();
    }
};

enum class TurnOutcome {
    success,
    provider_failure,
    internal_failure,
    disconnected,
};

void serve_chat(
    app::Application& application,
    const ParsedChatRequest& parsed,
    int fd,
    DaemonShutdown& shutdown,
    const TurnClock& clock) {
    bool headers_sent = false;
    try {
        if (shutdown.requested()) {
            shutdown.request_stop();
            return;
        }
        if (peer_closed(fd)) return;

        const std::uint64_t epoch = application.context_epoch();
        if (epoch == 0) {
            write_error(
                fd,
                make_error(
                    500,
                    "The application is unavailable.",
                    "server_error",
                    "internal_error"),
                shutdown.stop);
            return;
        }

        CreatedSessionCleanup created_cleanup{
            application, parsed.model, {}, epoch, shutdown};
        bool created = false;
        std::string session_id;
        if (!parsed.tag) {
            const CreateSessionSuccess created_session =
                application.create_session(parsed.model, "", epoch);
            session_id = created_session.id;
            created = true;
            created_cleanup.session_id = session_id;
            created_cleanup.remove = true;
        } else {
            session_id = parsed.tag->session_id;
        }

        const auto opened =
            application.open_session(parsed.model, session_id, epoch);
        if (const auto* error = std::get_if<ErrorCode>(&opened)) {
            const int status = *error == ErrorCode::not_found ? 404 : 500;
            write_error(
                fd,
                make_error(
                    status,
                    std::string(failure_text(*error)),
                    status == 404 ? "invalid_request_error" : "server_error",
                    status == 404 ? "session_not_found" : "internal_error"),
                shutdown.stop);
            return;
        }

        static std::atomic<std::uint64_t> next_subscription{1};
        const std::uint64_t subscription_number =
            next_subscription.fetch_add(1);
        SubscriptionCleanup cleanup{
            application,
            parsed.model,
            session_id,
            "daemon-" + std::to_string(subscription_number),
            "sub-" + std::to_string(subscription_number),
            epoch,
            shutdown,
        };
        const auto subscribed = application.subscribe(
            parsed.model,
            session_id,
            SubscribeCommand{
                cleanup.connection_id,
                epoch,
                cleanup.subscription_id},
            epoch);
        const auto* subscription = std::get_if<SubscribeResult>(&subscribed);
        if (subscription == nullptr || !subscription->session) {
            const ErrorCode code = std::holds_alternative<ErrorCode>(subscribed)
                ? std::get<ErrorCode>(subscribed)
                : ErrorCode::internal_error;
            if (code == ErrorCode::command_timeout
                || code == ErrorCode::application_unavailable
                || code == ErrorCode::server_stopping
                || !std::holds_alternative<ErrorCode>(subscribed)) {
                shutdown.request_stop();
            }
            write_error(
                fd,
                make_error(
                    500,
                    std::string(failure_text(code)),
                    "server_error",
                    "internal_error"),
                shutdown.stop);
            return;
        }
        cleanup.subscribed = true;
        cleanup.session = subscription->session;

        auto fail = [&](int status, std::string message, std::string code) {
            if (headers_sent) {
                const std::string body =
                    openai_error(message, "server_error", code).dump();
                (void)write_bytes(
                    fd, "data: " + body + "\n\n", shutdown.stop);
                return;
            }
            write_error(
                fd,
                make_error(
                    status,
                    std::move(message),
                    "server_error",
                    std::move(code)),
                shutdown.stop);
        };

        EntryId baseline = 0;
        bool saw_baseline = false;
        const auto baseline_deadline =
            steady_now(clock) + application.settings().command_deadline;
        while (!saw_baseline) {
            if (shutdown.requested()) {
                shutdown.request_stop();
                return;
            }
            if (peer_closed(fd)) return;
            if (auto item = cleanup.session->take_output()) {
                if (item->kind == app::SessionOutputItem::Kind::snapshot) {
                    baseline = highest_entry_id(item->snapshot);
                    saw_baseline = true;
                }
                cleanup.session->acknowledge_output();
            } else if (cleanup.session->output()->closed()
                || cleanup.session->lifecycle()
                    == LiveSessionState::finished) {
                fail(500, "The session ended before the turn finished.",
                    "internal_error");
                return;
            } else if (steady_now(clock) >= baseline_deadline) {
                fail(500, "The session did not become ready.",
                    "internal_error");
                return;
            } else {
                std::this_thread::sleep_for(poll_interval);
            }
        }

        if (shutdown.requested()) {
            shutdown.request_stop();
            return;
        }
        if (peer_closed(fd)) return;

        // Once submit starts, an exception or timeout leaves acceptance
        // uncertain. Keep the session until the process has safely drained.
        if (created) created_cleanup.remove = false;
        const CommandSubmitResult submitted = application.submit(
            parsed.model, session_id, RawCommand{parsed.user_text}, epoch);
        if (shutdown.requested()) shutdown.request_stop();

        if (const auto* code = std::get_if<ErrorCode>(&submitted)) {
            if (*code == ErrorCode::command_timeout) {
                // The command can still run after this result.
                write_error(
                    fd,
                    make_error(
                        500,
                        std::string(failure_text(*code)),
                        "server_error",
                        "command_timeout"),
                    shutdown.stop);
                shutdown.request_stop();
            } else {
                if (created) created_cleanup.remove = true;
                const int status =
                    *code == ErrorCode::not_found ? 404 : 500;
                write_error(
                    fd,
                    make_error(
                        status,
                        std::string(failure_text(*code)),
                        status == 404
                            ? "invalid_request_error"
                            : "server_error",
                        status == 404 ? "session_not_found"
                                      : "internal_error"),
                    shutdown.stop);
                if (*code != ErrorCode::not_found) return;
                return;
            }
        } else if (const auto* failure =
                       std::get_if<CommandFailure>(&submitted)) {
            if (created) created_cleanup.remove = true;
            if (failure->code == ErrorCode::invalid_argument) {
                const std::string message = failure->message.empty()
                    ? "The input was rejected."
                    : failure->message;
                write_error(
                    fd,
                    make_error(
                        400,
                        message,
                        "invalid_request_error",
                        "invalid_request"),
                    shutdown.stop);
                return;
            }
            fail(500,
                failure->message.empty()
                    ? std::string(failure_text(failure->code))
                    : failure->message,
                "internal_error");
            return;
        } else if (const auto* result =
                       std::get_if<CommandResult>(&submitted)) {
            if (!result->session.input_consumed) {
                if (created) created_cleanup.remove = true;
                const std::string message =
                    result->session.notice && !result->session.notice->empty()
                    ? *result->session.notice
                    : "The input was rejected.";
                write_error(
                    fd,
                    make_error(
                        400,
                        message,
                        "invalid_request_error",
                        "invalid_request"),
                    shutdown.stop);
                return;
            }
        } else {
            shutdown.request_stop();
            fail(500, "The request could not be completed.",
                "internal_error");
            return;
        }

        const bool accepted =
            std::holds_alternative<CommandResult>(submitted);
        if (!accepted) {
            // Submission timed out. Drain, then leave the process stopped.
        }

        const std::string id = completion_id();
        const std::int64_t created_at = unix_now();
        const std::string tag =
            format_session_tag(parsed.model, session_id);
        auto last_output = steady_now(clock);

        auto write_data = [&](const nlohmann::json& payload) {
            return write_bytes(
                fd, "data: " + payload.dump() + "\n\n", shutdown.stop);
        };
        auto chunk = [&](nlohmann::json delta, const char* finish) {
            nlohmann::json choice{
                {"index", 0},
                {"delta", std::move(delta)},
                {"finish_reason",
                 finish == nullptr ? nlohmann::json(nullptr)
                                   : nlohmann::json(finish)},
            };
            return nlohmann::json{
                {"id", id},
                {"object", "chat.completion.chunk"},
                {"created", created_at},
                {"model", parsed.model},
                {"choices", nlohmann::json::array({std::move(choice)})},
            };
        };

        bool draining = !accepted || shutdown.requested() || peer_closed(fd);
        bool cancel_sent = false;
        bool sent_reply_text = false;
        TurnOutcome outcome = draining
            ? TurnOutcome::disconnected
            : TurnOutcome::internal_failure;
        std::vector<Reply> replies;
        std::set<EntryId> logged_mismatch;
        SessionSnapshot latest{};

        auto note_mismatch = [&](EntryId entry_id) {
            if (!logged_mismatch.insert(entry_id).second) return;
            log_warn(
                "Streaming reply prefix changed; this reply will not send "
                "more text");
        };
        auto start_drain = [&](bool stop_process) {
            if (stop_process) shutdown.request_stop();
            else shutdown.arm_budget();
            draining = true;
            outcome = TurnOutcome::disconnected;
            if (cancel_sent) return;
            cancel_sent = true;
            const auto error = enqueue_cleanup_command(
                application,
                parsed.model,
                session_id,
                StopCommand{},
                epoch,
                shutdown);
            if (error && !harmless_cleanup_error(*error)) {
                shutdown.request_stop();
            }
        };
        if (draining) start_drain(shutdown.requested() || !accepted);

        if (!draining && parsed.stream) {
            const bool wrote_headers = write_bytes(
                fd,
                "Status: 200 OK\r\nContent-Type: text/event-stream\r\n\r\n",
                shutdown.stop);
            nlohmann::json delta{
                {"role", "assistant"},
                {"content", tag + "\n\n"},
            };
            if (!wrote_headers || !write_data(chunk(std::move(delta), nullptr))) {
                start_drain(false);
            } else {
                headers_sent = true;
            }
        }

        auto emit_replies = [&]() {
            if (!parsed.stream || draining || !headers_sent) return true;
            bool previous_started = false;
            for (Reply& reply : replies) {
                if (reply.kind != EntryKind::character) continue;
                if (reply.stopped) {
                    if (!reply.emitted.empty()) previous_started = true;
                    continue;
                }
                if (!reply.text.starts_with(reply.emitted)) {
                    if (!reply.emitted.empty()) note_mismatch(reply.id);
                    reply.stopped = true;
                    if (!reply.emitted.empty()) previous_started = true;
                    continue;
                }
                const std::string suffix =
                    reply.text.substr(reply.emitted.size());
                if (suffix.empty()) {
                    if (!reply.emitted.empty()) previous_started = true;
                    continue;
                }
                std::string piece;
                if (reply.emitted.empty()) {
                    if (previous_started) piece += "\n\n";
                    piece += "**";
                    piece += reply.display_name;
                    piece += ":** ";
                }
                piece += suffix;
                reply.emitted = reply.text;
                previous_started = true;
                sent_reply_text = true;
                last_output = steady_now(clock);
                if (!write_data(chunk(
                        nlohmann::json{{"content", piece}}, nullptr))) {
                    return false;
                }
            }
            return true;
        };

        auto apply_snapshot = [&](const SessionSnapshot& snapshot) {
            latest = snapshot;
            std::vector<Reply> next;
            std::set<EntryId> seen;
            for (const TranscriptEntry& entry : snapshot.transcript) {
                if (entry.id <= baseline) continue;
                seen.insert(entry.id);
                Reply reply{
                    .id = entry.id,
                    .kind = entry.kind,
                    .display_name = entry.display_name,
                    .text = entry.text,
                };
                if (const Reply* previous = find_reply(replies, entry.id)) {
                    reply.emitted = previous->emitted;
                    reply.stopped = previous->stopped;
                    if (parsed.stream && !reply.stopped
                        && !reply.emitted.empty()
                        && !reply.text.starts_with(reply.emitted)) {
                        note_mismatch(reply.id);
                        reply.stopped = true;
                    }
                }
                next.push_back(std::move(reply));
            }
            if (parsed.stream) {
                for (const Reply& old : replies) {
                    if (old.kind == EntryKind::character
                        && !old.emitted.empty()
                        && !seen.contains(old.id)) {
                        note_mismatch(old.id);
                    }
                }
            }
            replies = std::move(next);
        };

        while (true) {
            if (shutdown.requested() && !draining) start_drain(true);
            if (!draining && peer_closed(fd)) start_drain(false);
            if (draining && shutdown.expired()) {
                shutdown.request_stop();
                outcome = TurnOutcome::internal_failure;
                break;
            }
            if (parsed.stream && headers_sent && !draining
                && steady_now(clock) - last_output >= keepalive_interval) {
                if (!write_bytes(
                        fd, ": keepalive\n\n", shutdown.stop)) {
                    start_drain(false);
                    continue;
                }
                last_output = steady_now(clock);
            }

            auto item = cleanup.session->take_output();
            if (!item) {
                const bool closed = cleanup.session->output()->closed()
                    || cleanup.session->lifecycle()
                        == LiveSessionState::finished
                    || cleanup.session->lifecycle()
                        == LiveSessionState::stopping;
                if (closed) {
                    if (draining) {
                        if (!shutdown.requested()) {
                            cleanup.disarm_after_cleanup = true;
                        }
                    } else {
                        outcome = TurnOutcome::internal_failure;
                    }
                    break;
                }
                std::this_thread::sleep_for(poll_interval);
                continue;
            }

            const bool snapshot =
                item->kind == app::SessionOutputItem::Kind::snapshot;
            if (snapshot) apply_snapshot(item->snapshot);
            else if (const auto* target =
                         std::get_if<EntryTextTarget>(&item->target)) {
                if (Reply* reply = find_reply(replies, target->entry_id)) {
                    if (!reply->stopped) reply->text += item->text;
                }
            }
            cleanup.session->acknowledge_output();
            if (!emit_replies()) {
                start_drain(false);
                continue;
            }

            if (!snapshot) continue;
            const bool terminal =
                latest.lifecycle == SessionLifecycle::stopping
                || latest.shutdown_reason.has_value();
            const bool idle = !latest.generation.active;
            const bool has_new = std::any_of(
                latest.transcript.begin(),
                latest.transcript.end(),
                [&](const TranscriptEntry& entry) {
                    return entry.id > baseline;
                });
            if (draining && (idle || terminal)) {
                if (!shutdown.requested()) {
                    cleanup.disarm_after_cleanup = true;
                }
                break;
            }
            if (terminal) {
                outcome = TurnOutcome::internal_failure;
                break;
            }
            if (!draining && idle && has_new) {
                const bool provider_failed = std::any_of(
                    replies.begin(),
                    replies.end(),
                    [](const Reply& reply) {
                        return reply.kind == EntryKind::error;
                    });
                outcome = provider_failed
                    ? TurnOutcome::provider_failure
                    : TurnOutcome::success;
                break;
            }
        }

        if (outcome == TurnOutcome::disconnected) return;

        const std::string body = render_replies(replies);
        if (outcome == TurnOutcome::success && body.empty()) {
            outcome = TurnOutcome::internal_failure;
        }
        if (outcome == TurnOutcome::provider_failure) {
            fail(502, provider_failure_message(replies), "provider_error");
            return;
        }
        if (outcome == TurnOutcome::internal_failure) {
            fail(500, "The session ended before the turn finished.",
                "internal_error");
            return;
        }

        if (parsed.stream) {
            if (!sent_reply_text
                && !write_data(chunk(
                    nlohmann::json{{"content", body}}, nullptr))) {
                return;
            }
            if (!write_data(chunk(nlohmann::json::object(), "stop"))) return;
            (void)write_bytes(fd, "data: [DONE]\n\n", shutdown.stop);
            return;
        }

        write_json(
            fd,
            200,
            nlohmann::json{
                {"id", id},
                {"object", "chat.completion"},
                {"created", created_at},
                {"model", parsed.model},
                {"choices",
                 nlohmann::json::array({nlohmann::json{
                     {"index", 0},
                     {"message",
                      {{"role", "assistant"},
                       {"content", tag + "\n\n" + body}}},
                     {"finish_reason", "stop"},
                 }})},
            },
            shutdown.stop);
    } catch (const std::exception& error) {
        log_error(error.what());
        constexpr std::string_view message =
            "The request could not be completed.";
        if (headers_sent) {
            const std::string body = openai_error(
                message, "server_error", "internal_error").dump();
            (void)write_bytes(fd, "data: " + body + "\n\n", shutdown.stop);
        } else {
            write_error(
                fd,
                make_error(
                    500, std::string(message), "server_error", "internal_error"),
                shutdown.stop);
        }
        shutdown.request_stop();
    }
}

} // namespace

DaemonShutdown::DaemonShutdown(
    std::atomic<bool>& stop_flag,
    std::chrono::milliseconds shutdown_grace)
    : stop(stop_flag), grace(shutdown_grace) {}

void DaemonShutdown::arm_budget() {
    if (armed) return;
    deadline = std::chrono::steady_clock::now() + grace;
    armed = true;
}

void DaemonShutdown::request_stop() {
    stop.store(true, std::memory_order_relaxed);
    arm_budget();
}

void DaemonShutdown::disarm_if_idle() {
    if (stop.load(std::memory_order_relaxed)) return;
    armed = false;
    deadline = std::chrono::steady_clock::time_point::max();
}

bool DaemonShutdown::requested() const {
    return stop.load(std::memory_order_relaxed);
}

bool DaemonShutdown::expired() const {
    return armed && std::chrono::steady_clock::now() >= deadline;
}

std::chrono::milliseconds DaemonShutdown::remaining() const {
    if (!armed) return grace;
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return std::chrono::milliseconds::zero();
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - now);
}

nlohmann::json openai_error(
    std::string_view message,
    std::string_view type,
    std::string_view code) {
    return {
        {"error",
         {{"message", message}, {"type", type}, {"code", code}}},
    };
}

nlohmann::json models_list(app::Application& application) {
    nlohmann::json data = nlohmann::json::array();
    for (const ForumSummary& forum :
         application.bootstrap().presentation.forums) {
        if (forum.id == entrance_id) continue;
        data.push_back({
            {"id", forum.id},
            {"object", "model"},
            {"created", 0},
            {"owned_by", "cha"},
        });
    }
    return {{"object", "list"}, {"data", std::move(data)}};
}

std::variant<ParsedChatRequest, ChatParseError> parse_chat_request(
    std::string_view body, app::Application& application) {
    nlohmann::json request;
    try {
        request = nlohmann::json::parse(body);
    } catch (const nlohmann::json::parse_error&) {
        return make_error(
            400,
            "Malformed JSON",
            "invalid_request_error",
            "invalid_request");
    }
    if (!request.is_object()) {
        return make_error(
            400,
            "Request body must be a JSON object",
            "invalid_request_error",
            "invalid_request");
    }
    if (!request.contains("model") || !request["model"].is_string()) {
        return make_error(
            400,
            "Request must include a string model",
            "invalid_request_error",
            "invalid_request");
    }
    if (!request.contains("messages") || !request["messages"].is_array()
        || request["messages"].empty()) {
        return make_error(
            400,
            "Request must include a non-empty messages array",
            "invalid_request_error",
            "invalid_request");
    }
    bool stream = false;
    if (request.contains("stream")) {
        if (!request["stream"].is_boolean()) {
            return make_error(
                400,
                "The stream field must be a boolean",
                "invalid_request_error",
                "invalid_request");
        }
        stream = request["stream"].get<bool>();
    }

    const nlohmann::json& messages = request["messages"];
    const nlohmann::json& last = messages.back();
    if (!last.is_object() || !last.contains("role")
        || last["role"] != "user") {
        return make_error(
            400,
            "The last message must be a user message",
            "invalid_request_error",
            "invalid_request");
    }
    std::string user_text = message_text(last);
    if (user_text.empty()) {
        return make_error(
            400,
            "The user message has no usable text",
            "invalid_request_error",
            "invalid_request");
    }
    if (user_text.size() > application.settings().prompt_limit) {
        return make_error(
            400,
            "The input exceeds the prompt limit",
            "invalid_request_error",
            "invalid_request");
    }

    const std::string model = request["model"].get<std::string>();
    if (!model_is_exposed(application, model)) {
        return make_error(
            404,
            "The model '" + model + "' does not exist",
            "invalid_request_error",
            "model_not_found");
    }

    const SessionTagScanResult scan = find_session_tag(messages);
    ParsedChatRequest parsed{
        .model = model,
        .user_text = std::move(user_text),
        .stream = stream,
        .tag = std::nullopt,
    };
    if (scan.status == SessionTagScan::no_assistant) {
        return parsed;
    }
    if (scan.status == SessionTagScan::missing) {
        return make_error(
            400,
            std::string(missing_session_message),
            "invalid_request_error",
            "invalid_request");
    }
    if (scan.tag.forum_id != model) {
        return make_error(
            400,
            "This chat belongs to a different CHA forum",
            "invalid_request_error",
            "invalid_request");
    }
    parsed.tag = std::move(scan.tag);
    return parsed;
}

void handle_request(
    app::Application& application,
    const ScgiRequest& request,
    int fd,
    DaemonShutdown& shutdown,
    const TurnClock& clock) {
    try {
        if (shutdown.requested()) {
            shutdown.request_stop();
            return;
        }
        if (request.method == "GET" && request.document_uri == models_path) {
            write_json(fd, 200, models_list(application), shutdown.stop);
            return;
        }
        if (request.method == "POST" && request.document_uri == chat_path) {
            const auto parsed = parse_chat_request(request.body, application);
            if (const auto* error = std::get_if<ChatParseError>(&parsed)) {
                write_error(fd, *error, shutdown.stop);
                return;
            }
            serve_chat(
                application,
                std::get<ParsedChatRequest>(parsed),
                fd,
                shutdown,
                clock);
            return;
        }
        write_error(
            fd,
            make_error(
                404,
                "Unknown endpoint",
                "invalid_request_error",
                "not_found"),
            shutdown.stop);
    } catch (const std::exception& error) {
        log_error(error.what());
        write_error(
            fd,
            make_error(
                500,
                "The request could not be completed.",
                "server_error",
                "internal_error"),
            shutdown.stop);
    }
}

} // namespace cha::daemon
