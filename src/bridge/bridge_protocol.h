#pragma once

#include "web/protocol.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace cha::bridge {

inline constexpr int kProtocolVersion = 1;
inline constexpr std::uint64_t kMaxSafeInteger = 9007199254740991ULL;
inline constexpr std::string_view kApplicationVersion = "0";

enum class Method {
    bridge_info,
    app_bootstrap,
    session_create,
    session_open,
    session_submit,
    session_stop,
    session_close,
    session_snapshot,
    session_subscribe,
    session_unsubscribe,
    vault_list,
    vault_create,
    vault_update,
    vault_delete,
    vault_switch,
    vault_merge,
    vault_r2_list,
    vault_r2_download,
};

struct ParsedRequest {
    std::string connection_id;
    std::uint64_t id{};
    std::uint64_t context_epoch{};
    Method method{Method::bridge_info};
    nlohmann::json params = nlohmann::json::object();
};

struct ParseFailure {
    std::optional<std::uint64_t> id;
    cha::web::ErrorCode code{cha::web::ErrorCode::invalid_argument};
    std::string message;
};

struct DeliveryAck {
    std::string connection_id;
    std::uint64_t delivery_id{};
};

[[nodiscard]] bool is_control_method(Method method) noexcept;
[[nodiscard]] bool requires_context_epoch(Method method) noexcept;
[[nodiscard]] std::string_view method_name(Method method) noexcept;
[[nodiscard]] std::optional<Method> method_from_name(std::string_view name) noexcept;
[[nodiscard]] std::optional<std::uint64_t> as_safe_uint(const nlohmann::json& value);
[[nodiscard]] std::string_view public_error_message(cha::web::ErrorCode code) noexcept;

[[nodiscard]] std::variant<ParsedRequest, ParseFailure> parse_request(
    std::string_view text,
    std::size_t maximum_bytes);
[[nodiscard]] std::variant<DeliveryAck, ParseFailure> parse_ack(
    std::string_view text,
    std::size_t maximum_bytes);

[[nodiscard]] nlohmann::json reply_ok(
    std::string_view connection_id,
    std::uint64_t id,
    std::uint64_t context_epoch,
    nlohmann::json result);
[[nodiscard]] nlohmann::json reply_error(
    std::string_view connection_id,
    std::uint64_t id,
    std::uint64_t context_epoch,
    cha::web::ErrorCode code,
    std::string_view message);
[[nodiscard]] nlohmann::json bridge_info_result(std::string_view platform);
[[nodiscard]] nlohmann::json session_event(
    std::string_view connection_id,
    std::uint64_t context_epoch,
    std::string_view subscription_id,
    std::string_view event,
    std::string_view forum_id,
    std::string_view session_id,
    std::uint64_t seq,
    nlohmann::json payload);
[[nodiscard]] nlohmann::json delivery_batch(
    std::string_view connection_id,
    std::uint64_t delivery_id,
    std::vector<nlohmann::json> messages);
[[nodiscard]] nlohmann::json context_changed_event(
    std::string_view connection_id,
    std::uint64_t context_epoch,
    std::string_view state);

} // namespace cha::bridge
