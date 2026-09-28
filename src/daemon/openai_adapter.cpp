#include "daemon/openai_adapter.h"

#include "workspace/builtins.h"

#include <exception>
#include <utility>

namespace cha::daemon {
namespace {

constexpr std::string_view models_path = "/v1/models";
constexpr std::string_view chat_path = "/v1/chat/completions";
constexpr std::string_view missing_session_message =
    "This chat has no CHA session. Start a new chat.";

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

} // namespace

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
    std::atomic<bool>& stop) {
    try {
        if (request.method == "GET" && request.document_uri == models_path) {
            write_json(fd, 200, models_list(application), stop);
            return;
        }
        if (request.method == "POST" && request.document_uri == chat_path) {
            const auto parsed = parse_chat_request(request.body, application);
            if (const auto* error = std::get_if<ChatParseError>(&parsed)) {
                write_error(fd, *error, stop);
                return;
            }
            write_error(
                fd,
                make_error(
                    501,
                    "Chat completion is not implemented",
                    "server_error",
                    "not_implemented"),
                stop);
            return;
        }
        write_error(
            fd,
            make_error(
                404,
                "Unknown endpoint",
                "invalid_request_error",
                "not_found"),
            stop);
    } catch (const std::exception& error) {
        write_error(
            fd,
            make_error(
                500,
                error.what(),
                "server_error",
                "internal_error"),
            stop);
    }
}

} // namespace cha::daemon
