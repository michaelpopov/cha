#pragma once

#include "runtime/protocol.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace cha::bridge {

#ifndef CHA_PACKAGE_VERSION
#define CHA_PACKAGE_VERSION "development"
#endif

inline constexpr int kProtocolVersion = 1;
inline constexpr std::uint64_t kMaxSafeInteger = 9007199254740991ULL;
inline constexpr std::string_view kApplicationVersion = CHA_PACKAGE_VERSION;

enum class Method {
    bridge_info,
    app_bootstrap,
    session_create,
    session_list,
    session_rename,
    session_delete,
    session_open,
    session_submit,
    session_stop,
    session_close,
    session_snapshot,
    session_cover,
    session_uncover,
    session_delete_turn,
    session_set_default_character,
    session_export,
    session_subscribe,
    session_unsubscribe,
    character_get,
    character_create,
    character_update,
    character_update_definition,
    character_delete,
    character_file_get,
    character_file_create,
    character_file_update,
    character_file_delete,
    persona_get,
    persona_create,
    persona_update,
    persona_delete,
    forum_get,
    forum_create,
    forum_update,
    forum_delete,
    forum_members_update,
    forum_file_get,
    forum_file_create,
    forum_file_update,
    forum_file_delete,
    vault_list,
    vault_create,
    vault_update,
    vault_delete,
    vault_switch,
    vault_merge,
    vault_upload,
    vault_upload_check,
    vault_download,
    vault_import,
    vault_export,
    vault_r2_list,
    vault_r2_download,
    provider_list,
    provider_get,
    provider_create,
    provider_update,
    provider_delete,
    provider_test,
    style_list,
    style_create,
    style_update,
    style_delete,
    voice_list,
    voice_create,
    voice_update,
    voice_delete,
    voice_input_get,
    voice_input_save,
    voice_input_runtime,
    voice_input_connect,
    voice_input_cancel,
    voice_input_xai_start,
    voice_input_xai_audio,
    voice_input_xai_stop,
    voice_input_xai_cancel,
    voice_output_get,
    voice_output_save,
    voice_output_runtime,
    speech_start,
    speech_cancel,
    speech_release,
    audio_start,
    audio_start_batch,
    audio_status,
    audio_source,
    audio_clear_cache,
    audio_release,
    api_key_list,
    api_key_create,
    api_key_rename,
    api_key_replace_value,
    api_key_delete,
    r2_storage_get,
    r2_storage_save,
    r2_storage_delete,
    openai_auth_get,
    openai_auth_start,
    openai_auth_poll,
    openai_auth_disconnect,
    count,
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
    ErrorCode code{ErrorCode::invalid_argument};
    std::string message;
};

struct DeliveryAck {
    std::string connection_id;
    std::uint64_t delivery_id{};
};

[[nodiscard]] bool is_control_method(Method method) noexcept;
[[nodiscard]] bool requires_context_epoch(Method method) noexcept;
[[nodiscard]] bool changes_context(Method method) noexcept;
[[nodiscard]] std::string_view method_name(Method method) noexcept;
[[nodiscard]] std::optional<Method> method_from_name(std::string_view name) noexcept;
[[nodiscard]] std::optional<std::uint64_t> as_safe_uint(const nlohmann::json& value);
[[nodiscard]] std::string_view public_error_message(ErrorCode code) noexcept;

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
    ErrorCode code,
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
    std::string_view state,
    std::optional<std::uint64_t> causing_request_id = std::nullopt);
[[nodiscard]] nlohmann::json connection_invalidated_event(
    std::string_view connection_id);

} // namespace cha::bridge
