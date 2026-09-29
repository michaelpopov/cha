#include "daemon/openai_adapter.h"

#include "util/logging.h"
#include "workspace/builtins.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <exception>
#include <thread>
#include <utility>
#include <vector>

namespace cha::daemon {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::string_view models_path = "/v1/models";
constexpr std::string_view chat_path = "/v1/chat/completions";
constexpr auto poll_interval = std::chrono::milliseconds{25};
constexpr auto keepalive_interval = std::chrono::seconds{15};

ApiError bad_request(std::string message) {
    return {
        .status = 400,
        .message = std::move(message),
        .code = "invalid_request",
    };
}

ApiError server_error(
    std::string message, std::string code = "internal_error") {
    return {
        .status = 500,
        .message = std::move(message),
        .code = std::move(code),
    };
}

std::string_view failure_text(ErrorCode code) {
    switch (code) {
    case ErrorCode::not_found:
        return "That forum or session was not found.";
    case ErrorCode::invalid_argument:
        return "The input was rejected.";
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

ApiError error_for(ErrorCode code) {
    if (code == ErrorCode::not_found) {
        return {
            .status = 404,
            .message = std::string(failure_text(code)),
            .code = "session_not_found",
        };
    }
    return server_error(std::string(failure_text(code)));
}

bool write_json(
    int fd,
    int status,
    const nlohmann::json& body,
    const std::atomic<bool>& stop) {
    return write_cgi(fd, status, "application/json", body.dump(), stop);
}

std::uint64_t next_number() {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1);
}

std::int64_t unix_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// One validated chat request.
struct Chat {
    app::Application& application;
    const ParsedChatRequest& request;
    int fd{};
    std::atomic<bool>& stop;
    const TurnClock& clock;
    std::uint64_t epoch{};
    std::string session_id;
    bool new_session{};
    std::string title;
    bool title_sent{};
    std::string id{"chatcmpl-" + std::to_string(next_number())};
    std::int64_t created{unix_now()};
    bool streaming{};  // SSE headers were sent
    Clock::time_point last_write{};

    [[nodiscard]] Clock::time_point now() const {
        return clock ? clock() : Clock::now();
    }
};

bool send(Chat& chat, std::string_view bytes) {
    if (!write_bytes(chat.fd, bytes, chat.stop)) return false;
    chat.last_write = chat.now();
    return true;
}

bool send_event(Chat& chat, const nlohmann::json& payload) {
    return send(chat, "data: " + payload.dump() + "\n\n");
}

nlohmann::json chunk(
    const Chat& chat, nlohmann::json delta, const char* finish = nullptr) {
    nlohmann::json choice{
        {"index", 0},
        {"delta", std::move(delta)},
        {"finish_reason",
         finish == nullptr ? nlohmann::json(nullptr) : nlohmann::json(finish)},
    };
    return {
        {"id", chat.id},
        {"object", "chat.completion.chunk"},
        {"created", chat.created},
        {"model", chat.request.model_name},
        {"choices", nlohmann::json::array({std::move(choice)})},
    };
}

bool send_text(Chat& chat, std::string text) {
    return send_event(chat, chunk(chat, {{"content", std::move(text)}}));
}

// Before the SSE headers an error is a JSON response. After them it is the
// last SSE event.
void send_error(Chat& chat, const ApiError& error) {
    if (chat.streaming) (void)send_event(chat, openai_error(error));
    else (void)write_error(chat.fd, error, chat.stop);
}

bool start_stream(Chat& chat) {
    constexpr std::string_view headers =
        "Status: 200 OK\r\nContent-Type: text/event-stream\r\n\r\n";
    if (!send(chat, headers)) return false;
    chat.streaming = true;
    return send_event(chat, chunk(chat, {{"role", "assistant"}}));
}

bool title_ready(Chat& chat, const LiveSession& session) {
    if (!chat.new_session || !chat.title.empty()) return true;
    if (session.naming()) return false;
    for (const SessionListing& listed :
         chat.application.list_sessions(chat.request.model, chat.epoch)) {
        if (listed.id == chat.session_id) {
            chat.title = listed.label;
            return true;
        }
    }
    return false;
}

// A transcript entry added by the current turn.
struct Reply {
    EntryId id{};
    EntryKind kind{EntryKind::notice};
    std::string name;
    std::string text;
    std::string sent;  // streamed text, without the speaker label
    bool stopped{};    // a snapshot rewrote the sent text
};

struct Turn {
    EntryId baseline{};  // entries after this ID belong to the turn
    std::vector<Reply> replies;
    bool idle{};
    bool terminal{};
};

Reply* find_reply(std::vector<Reply>& replies, EntryId id) {
    for (Reply& reply : replies) {
        if (reply.id == id) return &reply;
    }
    return nullptr;
}

void log_prefix_change() {
    log_warn(
        "Streaming reply prefix changed; this reply will not send more text");
}

void apply_snapshot(Turn& turn, const SessionSnapshot& snapshot) {
    std::vector<Reply> next;
    for (const TranscriptEntry& entry : snapshot.transcript) {
        if (entry.id <= turn.baseline) continue;
        Reply reply{
            .id = entry.id,
            .kind = entry.kind,
            .name = entry.display_name,
            .text = entry.text,
        };
        if (const Reply* old = find_reply(turn.replies, entry.id)) {
            reply.sent = old->sent;
            reply.stopped = old->stopped;
        }
        // Appends only extend text; a snapshot can rewrite it.
        if (!reply.stopped && !reply.sent.empty()
            && (reply.kind != EntryKind::character
                || !reply.text.starts_with(reply.sent))) {
            log_prefix_change();
            reply.stopped = true;
        }
        next.push_back(std::move(reply));
    }
    for (const Reply& old : turn.replies) {
        if (!old.sent.empty() && !old.stopped
            && find_reply(next, old.id) == nullptr) {
            log_prefix_change();
        }
    }
    turn.replies = std::move(next);
    turn.idle = !snapshot.generation.active;
    turn.terminal = snapshot.lifecycle == SessionLifecycle::stopping
        || snapshot.shutdown_reason.has_value();
}

std::string label(const Reply& reply) {
    return "**" + reply.name + ":** ";
}

// Character replies in transcript order, each with its speaker label.
std::string render(const Turn& turn) {
    std::string text;
    bool human = false;
    for (const Reply& reply : turn.replies) {
        if (reply.kind == EntryKind::human) human = true;
        if (reply.kind != EntryKind::character || reply.text.empty()) continue;
        if (!text.empty()) text += "\n\n";
        text += label(reply) + reply.text;
    }
    if (text.empty() && human) return "(recorded)";
    return text;
}

// Sends the reply text that is new since the last call. SSE cannot take text
// back, so a stopped reply sends nothing more. Returns false when a write
// fails.
bool stream_replies(Chat& chat, Turn& turn) {
    bool started = false;
    for (Reply& reply : turn.replies) {
        if (reply.kind != EntryKind::character) continue;
        if (reply.stopped || reply.text.size() == reply.sent.size()) {
            if (!reply.sent.empty()) started = true;
            continue;
        }
        std::string piece;
        if (reply.sent.empty()) piece = (started ? "\n\n" : "") + label(reply);
        piece += reply.text.substr(reply.sent.size());
        reply.sent = reply.text;
        started = true;
        if (!send_text(chat, std::move(piece))) return false;
    }
    return true;
}

bool session_closed(const LiveSession& session) {
    const LiveSessionState state = session.lifecycle();
    return session.output()->closed() || state == LiveSessionState::stopping
        || state == LiveSessionState::finished;
}

enum class TurnEnd {
    complete,
    cancelled,  // the client left; nothing more to send
    failed,
};

// Consumes session output until the turn ends. When the client leaves, it
// stops the generation and waits until the session is idle, so the next
// request never finds the session busy. On SIGTERM it returns at once; the
// application shutdown then cancels the generation.
TurnEnd run_turn(Chat& chat, LiveSession& session, Turn& turn) {
    const bool stream = chat.request.stream;
    bool cancelled = false;
    auto drain_until = Clock::time_point::max();
    auto cancel = [&] {
        if (cancelled) return;
        cancelled = true;
        drain_until = Clock::now() + chat.application.settings().shutdown_grace;
        (void)chat.application.submit_async(
            chat.request.model, chat.session_id, StopCommand{}, chat.epoch);
    };

    if (stream && !start_stream(chat)) cancel();
    while (true) {
        if (chat.stop.load()) return TurnEnd::failed;
        if (!cancelled && peer_closed(chat.fd)) cancel();
        if (Clock::now() >= drain_until) {
            log_warn("A cancelled turn did not end; the daemon stops");
            chat.stop.store(true);
            return TurnEnd::failed;
        }
        if (stream && !cancelled
            && chat.now() - chat.last_write >= keepalive_interval
            && !send(chat, ": keepalive\n\n")) {
            cancel();
        }

        const auto item = session.take_output();
        if (!item) {
            if (session_closed(session)) {
                return cancelled ? TurnEnd::cancelled : TurnEnd::failed;
            }
            std::this_thread::sleep_for(poll_interval);
            continue;
        }
        const bool snapshot =
            item->kind == app::SessionOutputItem::Kind::snapshot;
        if (snapshot) {
            apply_snapshot(turn, item->snapshot);
        } else if (const auto* target =
                       std::get_if<EntryTextTarget>(&item->target)) {
            if (Reply* reply = find_reply(turn.replies, target->entry_id)) {
                reply->text += item->text;
            }
        }
        session.acknowledge_output();
        if (!cancelled && title_ready(chat, session)) {
            if (stream && chat.new_session && !chat.title.empty()
                && !chat.title_sent) {
                if (!send_text(chat, chat.title + "\n\n")) cancel();
                else chat.title_sent = true;
            }
            if (stream && !cancelled && !stream_replies(chat, turn)) cancel();
        }

        if (!snapshot) continue;
        if (cancelled && (turn.idle || turn.terminal)) {
            return TurnEnd::cancelled;
        }
        if (turn.terminal) return TurnEnd::failed;
        if (turn.idle && !turn.replies.empty()
            && title_ready(chat, session)) return TurnEnd::complete;
    }
}

void finish_turn(Chat& chat, TurnEnd end, const Turn& turn) {
    if (end == TurnEnd::cancelled) return;
    const std::string text = render(turn);
    const auto error = std::ranges::find(
        turn.replies, EntryKind::error, &Reply::kind);
    if (end == TurnEnd::complete && error != turn.replies.end()) {
        send_error(
            chat,
            {.status = 502,
             .message = error->text.empty() ? "The provider failed."
                                            : error->text,
             .code = "provider_error"});
    } else if (end == TurnEnd::failed || text.empty()) {
        send_error(chat, server_error("The turn did not finish."));
    } else if (chat.request.stream) {
        const bool sent = std::ranges::any_of(turn.replies, [](const Reply& r) {
            return !r.sent.empty();
        });
        const auto last = chunk(chat, nlohmann::json::object(), "stop");
        if ((sent || send_text(chat, text)) && send_event(chat, last)) {
            (void)send(chat, "data: [DONE]\n\n");
        }
    } else {
        nlohmann::json choice{
            {"index", 0},
            {"message",
             {{"role", "assistant"},
              {"content", (chat.new_session ? chat.title + "\n\n" : "") + text}}},
            {"finish_reason", "stop"},
        };
        (void)write_json(
            chat.fd,
            200,
            {
                {"id", chat.id},
                {"object", "chat.completion"},
                {"created", chat.created},
                {"model", chat.request.model_name},
                {"choices", nlohmann::json::array({std::move(choice)})},
            },
            chat.stop);
    }
}

// The first snapshot after subscribe. Entries after its highest ID belong to
// this turn.
std::optional<EntryId> wait_for_baseline(
    const Chat& chat, LiveSession& session) {
    const auto until =
        Clock::now() + chat.application.settings().command_deadline;
    while (Clock::now() < until && !session_closed(session)) {
        const auto item = session.take_output();
        if (!item) {
            std::this_thread::sleep_for(poll_interval);
            continue;
        }
        std::optional<EntryId> baseline;
        if (item->kind == app::SessionOutputItem::Kind::snapshot) {
            baseline = 0;
            for (const TranscriptEntry& entry : item->snapshot.transcript) {
                baseline = std::max(*baseline, entry.id);
            }
        }
        session.acknowledge_output();
        if (baseline) return baseline;
    }
    return std::nullopt;
}

enum class Submission {
    accepted,
    rejected,
    unknown,  // the command can still run later
};

// Submits the user text. Unless CHA accepted it, writes the error response.
Submission submit_input(Chat& chat) {
    const CommandSubmitResult result = chat.application.submit(
        chat.request.model,
        chat.session_id,
        RawCommand{chat.request.user_text},
        chat.epoch);
    // clear_input alone is not acceptance: an unknown command sets it too.
    if (const auto* done = std::get_if<CommandResult>(&result)) {
        if (done->session.input_consumed) return Submission::accepted;
        const auto& notice = done->session.notice;
        send_error(
            chat,
            bad_request(
                notice && !notice->empty() ? *notice
                                           : "The input was rejected."));
        return Submission::rejected;
    }
    if (const auto* failure = std::get_if<CommandFailure>(&result)) {
        std::string message = failure->message.empty()
            ? std::string(failure_text(failure->code))
            : failure->message;
        send_error(
            chat,
            failure->code == ErrorCode::invalid_argument
                ? bad_request(std::move(message))
                : server_error(std::move(message)));
        return Submission::rejected;
    }
    const auto* code = std::get_if<ErrorCode>(&result);
    if (code != nullptr && *code != ErrorCode::command_timeout) {
        send_error(chat, error_for(*code));
        return Submission::rejected;
    }
    // Do not serve more requests next to a command that can still run.
    send_error(
        chat,
        code != nullptr
            ? server_error(std::string(failure_text(*code)), "command_timeout")
            : server_error("The request could not be completed."));
    chat.stop.store(true);
    return Submission::unknown;
}

// Runs one turn in the subscribed session. Returns false when the input
// certainly did not reach the session.
bool run_subscribed(Chat& chat, LiveSession& session) {
    const std::optional<EntryId> baseline = wait_for_baseline(chat, session);
    if (!baseline) {
        send_error(chat, server_error("The session did not become ready."));
        return false;
    }
    if (chat.stop.load() || peer_closed(chat.fd)) return false;
    const Submission submitted = submit_input(chat);
    if (submitted != Submission::accepted) {
        return submitted == Submission::unknown;
    }
    Turn turn{.baseline = *baseline};
    finish_turn(chat, run_turn(chat, session, turn), turn);
    return true;
}

// Opens and subscribes to the session for one turn. Returns false when the
// input certainly did not reach the session.
bool run_in_session(Chat& chat) {
    const auto opened = chat.application.open_session(
        chat.request.model, chat.session_id, chat.epoch);
    if (const auto* code = std::get_if<ErrorCode>(&opened)) {
        send_error(chat, error_for(*code));
        return false;
    }
    const std::string number = std::to_string(next_number());
    const SubscribeCommand subscription{
        "daemon-" + number, chat.epoch, "sub-" + number};
    const auto subscribed = chat.application.subscribe(
        chat.request.model, chat.session_id, subscription, chat.epoch);
    const auto* result = std::get_if<SubscribeResult>(&subscribed);
    if (result == nullptr || !result->session) {
        // The session was just opened, so its state is not known.
        send_error(chat, server_error("The session output is not available."));
        chat.stop.store(true);
        return false;
    }
    const bool used = run_subscribed(chat, *result->session);
    // A failed unsubscribe is harmless: the next subscribe replaces it.
    (void)chat.application.submit_async(
        chat.request.model,
        chat.session_id,
        UnsubscribeCommand{
            subscription.connection_id,
            chat.epoch,
            subscription.subscription_id},
        chat.epoch);
    return used;
}

void serve_chat(
    app::Application& application,
    const ParsedChatRequest& request,
    int fd,
    std::atomic<bool>& stop,
    const TurnClock& clock) {
    if (stop.load() || peer_closed(fd)) return;
    Chat chat{
        .application = application,
        .request = request,
        .fd = fd,
        .stop = stop,
        .clock = clock,
        .epoch = application.context_epoch(),
    };
    try {
        if (chat.epoch == 0) {
            send_error(chat, server_error("The application is unavailable."));
            return;
        }
        const bool created = !request.tag && !request.title;
        chat.new_session = created;
        if (created) {
            chat.session_id =
                application.create_session(request.model, "", chat.epoch).id;
        } else if (request.tag) {
            chat.session_id = request.tag->session_id;
        } else {
            const auto sessions = application.list_sessions(request.model, chat.epoch);
            const SessionListing* selected = nullptr;
            for (const SessionListing& candidate : sessions) {
                if (candidate.label != *request.title) continue;
                if (!selected || candidate.updated_at > selected->updated_at
                    || (candidate.updated_at == selected->updated_at
                        && candidate.id > selected->id)) {
                    selected = &candidate;
                }
            }
            if (!selected) {
                send_error(chat, error_for(ErrorCode::not_found));
                return;
            }
            chat.session_id = selected->id;
        }
        if (run_in_session(chat) || !created) return;
        // CHA keeps a new session before it parses the input, so startup
        // pruning does not remove a session whose first input was rejected.
        const auto error = application.delete_session(
            request.model, chat.session_id, chat.epoch);
        if (error && *error != ErrorCode::not_found) {
            log_warn("Could not delete rejected session " + chat.session_id);
        }
    } catch (const std::exception& error) {
        // The session state is not known after an unexpected failure. Let
        // systemd start a new daemon for the next request.
        log_error(error.what());
        send_error(chat, server_error("The request could not be completed."));
        stop.store(true);
    }
}

} // namespace

nlohmann::json openai_error(const ApiError& error) {
    return {
        {"error",
         {{"message", error.message},
          {"type",
           error.status < 500 ? "invalid_request_error" : "server_error"},
          {"code", error.code}}},
    };
}

bool write_error(
    int fd, const ApiError& error, const std::atomic<bool>& stop) {
    return write_json(fd, error.status, openai_error(error), stop);
}

nlohmann::json models_list(app::Application& application) {
    nlohmann::json data = nlohmann::json::array();
    for (const ForumSummary& forum :
         application.bootstrap().presentation.forums) {
        if (forum.id == entrance_id) continue;
        data.push_back({
            {"id", forum.display_name},
            {"object", "model"},
            {"created", 0},
            {"owned_by", "cha"},
        });
    }
    return {{"object", "list"}, {"data", std::move(data)}};
}

std::variant<ParsedChatRequest, ApiError> parse_chat_request(
    std::string_view body, app::Application& application) {
    nlohmann::json request;
    try {
        request = nlohmann::json::parse(body);
    } catch (const nlohmann::json::parse_error&) {
        return bad_request("Malformed JSON");
    }
    if (!request.is_object()) {
        return bad_request("Request body must be a JSON object");
    }
    if (!request.contains("model") || !request["model"].is_string()) {
        return bad_request("Request must include a string model");
    }
    if (!request.contains("messages") || !request["messages"].is_array()
        || request["messages"].empty()) {
        return bad_request("Request must include a non-empty messages array");
    }
    bool stream = false;
    if (request.contains("stream")) {
        if (!request["stream"].is_boolean()) {
            return bad_request("The stream field must be a boolean");
        }
        stream = request["stream"].get<bool>();
    }

    const nlohmann::json& messages = request["messages"];
    const nlohmann::json& last = messages.back();
    if (!last.is_object() || !last.contains("role")
        || last["role"] != "user") {
        return bad_request("The last message must be a user message");
    }
    std::string user_text = message_text(last);
    if (user_text.empty()) {
        return bad_request("The user message has no usable text");
    }
    if (user_text.size() > application.settings().prompt_limit) {
        return bad_request("The input exceeds the prompt limit");
    }

    std::string model_name = request["model"].get<std::string>();
    std::string model;
    std::string legacy_model;
    for (const ForumSummary& forum :
         application.bootstrap().presentation.forums) {
        if (forum.id == entrance_id) continue;
        if (forum.display_name == model_name) model = forum.id;
        if (forum.id == model_name) legacy_model = forum.id;
    }
    if (model.empty()) model = std::move(legacy_model);
    if (model.empty()) {
        return ApiError{
            .status = 404,
            .message = "The model '" + model_name + "' does not exist",
            .code = "model_not_found",
        };
    }

    const SessionTagScanResult scan = find_session_tag(messages);
    const auto title = scan.status == SessionTagScan::found
        ? std::nullopt : first_assistant_title(messages);
    if (scan.status == SessionTagScan::missing && !title) {
        return bad_request("This chat has no CHA session. Start a new chat.");
    }
    if (scan.status == SessionTagScan::found && scan.tag.forum_id != model) {
        return bad_request("This chat belongs to a different CHA forum");
    }
    return ParsedChatRequest{
        .model = std::move(model),
        .model_name = std::move(model_name),
        .user_text = std::move(user_text),
        .stream = stream,
        .tag = scan.status == SessionTagScan::found
            ? std::optional<SessionTag>(scan.tag)
            : std::nullopt,
        .title = title,
    };
}

void handle_request(
    app::Application& application,
    const ScgiRequest& request,
    int fd,
    std::atomic<bool>& stop,
    const TurnClock& clock) {
    try {
        if (stop.load()) return;
        if (request.method == "GET" && request.document_uri == models_path) {
            write_json(fd, 200, models_list(application), stop);
            return;
        }
        if (request.method == "POST" && request.document_uri == chat_path) {
            const auto parsed = parse_chat_request(request.body, application);
            if (const auto* error = std::get_if<ApiError>(&parsed)) {
                write_error(fd, *error, stop);
                return;
            }
            serve_chat(
                application,
                std::get<ParsedChatRequest>(parsed),
                fd,
                stop,
                clock);
            return;
        }
        write_error(
            fd,
            {.status = 404, .message = "Unknown endpoint", .code = "not_found"},
            stop);
    } catch (const std::exception& error) {
        log_error(error.what());
        write_error(
            fd, server_error("The request could not be completed."), stop);
    }
}

} // namespace cha::daemon
