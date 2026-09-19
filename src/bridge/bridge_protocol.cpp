#include "bridge/bridge_protocol.h"

#include "util/path_name.h"

#include <utility>

namespace cha::bridge {
namespace {

const std::pair<Method, std::string_view> kMethods[] = {
    {Method::bridge_info, "bridge.info"},
    {Method::app_bootstrap, "app.bootstrap"},
    {Method::session_create, "session.create"},
    {Method::session_open, "session.open"},
    {Method::session_submit, "session.submit"},
    {Method::session_stop, "session.stop"},
    {Method::session_close, "session.close"},
    {Method::session_snapshot, "session.snapshot"},
    {Method::session_subscribe, "session.subscribe"},
    {Method::session_unsubscribe, "session.unsubscribe"},
};

bool has_only_keys(
    const nlohmann::json& json,
    std::initializer_list<std::string_view> keys) {
    if (!json.is_object()) return false;
    for (auto it = json.begin(); it != json.end(); ++it) {
        bool known = false;
        for (std::string_view key : keys) {
            if (it.key() == key) {
                known = true;
                break;
            }
        }
        if (!known) return false;
    }
    return true;
}

ParseFailure invalid(std::string message, std::optional<std::uint64_t> id = {}) {
    return {id, cha::web::ErrorCode::invalid_argument, std::move(message)};
}

} // namespace

bool is_control_method(Method method) noexcept {
    return method == Method::session_stop
        || method == Method::session_unsubscribe;
}

bool requires_context_epoch(Method method) noexcept {
    return method != Method::bridge_info && method != Method::app_bootstrap;
}

std::string_view method_name(Method method) noexcept {
    for (const auto& [value, name] : kMethods) {
        if (value == method) return name;
    }
    return "";
}

std::optional<Method> method_from_name(std::string_view name) noexcept {
    for (const auto& [value, method] : kMethods) {
        if (method == name) return value;
    }
    return std::nullopt;
}

std::optional<std::uint64_t> as_safe_uint(const nlohmann::json& value) {
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > kMaxSafeInteger) return std::nullopt;
        return number;
    }
    if (value.is_number_integer() && !value.is_number_float()) {
        const auto number = value.get<std::int64_t>();
        if (number < 0 || static_cast<std::uint64_t>(number) > kMaxSafeInteger) {
            return std::nullopt;
        }
        return static_cast<std::uint64_t>(number);
    }
    return std::nullopt;
}

std::string_view public_error_message(cha::web::ErrorCode code) noexcept {
    using cha::web::ErrorCode;
    switch (code) {
    case ErrorCode::not_found:
        return "That forum or session was not found.";
    case ErrorCode::invalid_argument:
        return "The request was not valid.";
    case ErrorCode::body_too_large:
        return "The request is too large.";
    case ErrorCode::prompt_too_large:
        return "Prompt is too large.";
    case ErrorCode::session_not_live:
        return "That session is not open.";
    case ErrorCode::session_stopping:
        return "The session is stopping.";
    case ErrorCode::session_limit_reached:
        return "Another session has not closed yet.";
    case ErrorCode::session_open_timeout:
        return "Opening the session timed out.";
    case ErrorCode::command_queue_full:
        return "The session command queue is full.";
    case ErrorCode::command_timeout:
        return "The command outcome is unknown.";
    case ErrorCode::operation_cancelled:
        return "The operation was cancelled.";
    case ErrorCode::vault_changed:
        return "The vault context has changed.";
    case ErrorCode::application_unavailable:
        return "The application is unavailable.";
    default:
        return "The request could not be completed.";
    }
}

std::variant<ParsedRequest, ParseFailure> parse_request(
    std::string_view text,
    std::size_t maximum_bytes) {
    if (text.size() > maximum_bytes) {
        return ParseFailure{
            std::nullopt,
            cha::web::ErrorCode::body_too_large,
            std::string(public_error_message(cha::web::ErrorCode::body_too_large))};
    }
    nlohmann::json json;
    try {
        json = nlohmann::json::parse(text.begin(), text.end());
    } catch (const nlohmann::json::exception&) {
        return invalid("The request was not valid JSON.");
    }
    if (!json.is_object()
        || !has_only_keys(
            json, {"connection_id", "id", "context_epoch", "method", "params"})) {
        return invalid("The request envelope is not valid.");
    }
    if (!json.contains("connection_id") || !json.contains("id")
        || !json.contains("context_epoch") || !json.contains("method")) {
        return invalid("The request envelope is missing a required field.");
    }
    if (!json["connection_id"].is_string()
        || !cha::is_url_safe_identifier(
            json["connection_id"].get_ref<const std::string&>())) {
        return invalid("The request envelope is not valid.");
    }
    const auto id = as_safe_uint(json["id"]);
    const auto epoch = as_safe_uint(json["context_epoch"]);
    if (!id || !epoch || !json["method"].is_string()) {
        return invalid("The request envelope is not valid.", id);
    }
    const auto method = method_from_name(
        json["method"].get_ref<const std::string&>());
    if (!method) {
        return ParseFailure{
            *id,
            cha::web::ErrorCode::invalid_argument,
            "That method is not available."};
    }
    nlohmann::json params = nlohmann::json::object();
    if (json.contains("params")) {
        if (!json["params"].is_object()) {
            return invalid("The request envelope is not valid.", id);
        }
        params = json["params"];
    }
    return ParsedRequest{
        json["connection_id"].get<std::string>(),
        *id,
        *epoch,
        *method,
        std::move(params)};
}

std::variant<DeliveryAck, ParseFailure> parse_ack(
    std::string_view text,
    std::size_t maximum_bytes) {
    if (text.size() > maximum_bytes) {
        return ParseFailure{
            std::nullopt,
            cha::web::ErrorCode::body_too_large,
            std::string(public_error_message(cha::web::ErrorCode::body_too_large))};
    }
    nlohmann::json json;
    try {
        json = nlohmann::json::parse(text.begin(), text.end());
    } catch (const nlohmann::json::exception&) {
        return invalid("The acknowledgement is not valid JSON.");
    }
    if (!json.is_object()
        || !has_only_keys(json, {"connection_id", "delivery_id"})
        || !json.contains("connection_id") || !json.contains("delivery_id")
        || !json["connection_id"].is_string()
        || !cha::is_url_safe_identifier(
            json["connection_id"].get_ref<const std::string&>())) {
        return invalid("The acknowledgement is not valid.");
    }
    const auto delivery_id = as_safe_uint(json["delivery_id"]);
    if (!delivery_id) return invalid("The acknowledgement is not valid.");
    return DeliveryAck{
        json["connection_id"].get<std::string>(), *delivery_id};
}

nlohmann::json reply_ok(
    std::string_view connection_id,
    std::uint64_t id,
    std::uint64_t context_epoch,
    nlohmann::json result) {
    return {
        {"connection_id", connection_id},
        {"id", id},
        {"context_epoch", context_epoch},
        {"ok", true},
        {"result", std::move(result)},
    };
}

nlohmann::json reply_error(
    std::string_view connection_id,
    std::uint64_t id,
    std::uint64_t context_epoch,
    cha::web::ErrorCode code,
    std::string_view message) {
    return {
        {"connection_id", connection_id},
        {"id", id},
        {"context_epoch", context_epoch},
        {"ok", false},
        {"error",
         {{"code", cha::web::to_string(code)}, {"message", message}}},
    };
}

nlohmann::json bridge_info_result(std::string_view platform) {
    return {
        {"protocol_version", kProtocolVersion},
        {"application_version", kApplicationVersion},
        {"platform", platform},
    };
}

nlohmann::json session_event(
    std::string_view connection_id,
    std::uint64_t context_epoch,
    std::string_view subscription_id,
    std::string_view event,
    std::string_view forum_id,
    std::string_view session_id,
    std::uint64_t seq,
    nlohmann::json payload) {
    return {
        {"connection_id", connection_id},
        {"context_epoch", context_epoch},
        {"subscription_id", subscription_id},
        {"event", event},
        {"forum_id", forum_id},
        {"session_id", session_id},
        {"seq", seq},
        {"payload", std::move(payload)},
    };
}

nlohmann::json delivery_batch(
    std::string_view connection_id,
    std::uint64_t delivery_id,
    std::vector<nlohmann::json> messages) {
    return {
        {"connection_id", connection_id},
        {"delivery_id", delivery_id},
        {"messages", std::move(messages)},
    };
}

} // namespace cha::bridge
