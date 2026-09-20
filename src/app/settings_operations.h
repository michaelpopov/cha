#pragma once

#include "providers/credentials.h"
#include "providers/openai_oauth.h"
#include "web/protocol.h"
#include "workspace/workspace.h"

#include <atomic>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace cha {
class ApiKeyStore;
class OpenAiOAuth;
class WorkspaceConfigStore;
}

namespace cha::web {
class LiveSessionManager;
}

namespace cha::app::settings {

[[nodiscard]] cha::web::ProviderSummary provider_summary(
    const WorkspaceProvider& provider);
[[nodiscard]] cha::web::ProviderDetail provider_detail(
    const WorkspaceProvider& provider,
    bool writable,
    std::vector<std::string> used_by,
    const ApiKeyStore& api_keys);
[[nodiscard]] cha::web::StyleDetail style_detail(
    const WorkspaceStyle& style,
    bool writable,
    std::vector<std::string> used_by);
[[nodiscard]] cha::web::VoiceDetail voice_detail(
    const WorkspaceVoice& voice,
    bool writable,
    std::vector<std::string> used_by);
[[nodiscard]] cha::web::ApiKeyDetail api_key_detail(
    const ApiKeyInfo& key,
    const Workspace& workspace);
[[nodiscard]] cha::web::R2StorageDetail r2_storage_detail(
    const R2StorageInfo& key);
[[nodiscard]] cha::web::OpenAiAuth openai_auth_from(
    const OpenAiOAuthSnapshot& snapshot);

[[nodiscard]] std::vector<cha::web::ProviderSummary> list_providers();
[[nodiscard]] cha::web::ProviderDetail get_provider(
    std::string_view id,
    const ApiKeyStore& api_keys);
[[nodiscard]] cha::web::ProviderDetail create_provider(
    WorkspaceConfigStore& store,
    const ApiKeyStore& api_keys,
    const cha::web::CreateProviderRequest& create);
[[nodiscard]] cha::web::ProviderDetail update_provider(
    WorkspaceConfigStore& store,
    cha::web::LiveSessionManager& live_sessions,
    const ApiKeyStore& api_keys,
    std::string_view id,
    const nlohmann::json& body);
void delete_provider(WorkspaceConfigStore& store, std::string_view id);
void test_provider(
    std::string_view id,
    const nlohmann::json& body,
    OpenAiOAuth& openai_auth,
    ApiKeyStore& api_keys,
    const std::atomic_bool& cancellation);

[[nodiscard]] std::vector<cha::web::StyleDetail> list_styles();
[[nodiscard]] cha::web::StyleDetail create_style(
    WorkspaceConfigStore& store,
    std::string_view display_name);
[[nodiscard]] cha::web::StyleDetail update_style(
    WorkspaceConfigStore& store,
    cha::web::LiveSessionManager& live_sessions,
    std::string_view id,
    const cha::web::StyleUpdate& update);
void delete_style(WorkspaceConfigStore& store, std::string_view id);

[[nodiscard]] std::vector<cha::web::VoiceDetail> list_voices();
[[nodiscard]] cha::web::VoiceDetail create_voice(
    WorkspaceConfigStore& store,
    const cha::web::CreateVoiceRequest& create);
[[nodiscard]] cha::web::VoiceDetail update_voice(
    WorkspaceConfigStore& store,
    cha::web::LiveSessionManager& live_sessions,
    std::string_view id,
    const cha::web::VoiceUpdate& update);
void delete_voice(WorkspaceConfigStore& store, std::string_view id);

[[nodiscard]] std::optional<cha::web::VoiceInputSettings> get_voice_input_settings();
[[nodiscard]] cha::web::VoiceInputSettings save_voice_input_settings(
    WorkspaceConfigStore& store,
    const ApiKeyStore& api_keys,
    const cha::web::VoiceInputSettings& update);
[[nodiscard]] std::optional<cha::web::VoiceInputRuntime> get_voice_input_runtime(
    const ApiKeyStore& api_keys,
    bool voice_enabled);
[[nodiscard]] std::optional<std::string> voice_input_secret(
    const ApiKeyStore& api_keys,
    bool voice_enabled);

[[nodiscard]] std::optional<cha::web::VoiceOutputSettings> get_voice_output_settings();
[[nodiscard]] cha::web::VoiceOutputSettings save_voice_output_settings(
    WorkspaceConfigStore& store,
    const ApiKeyStore& api_keys,
    const cha::web::VoiceOutputSettings& update);
[[nodiscard]] std::optional<cha::web::VoiceOutputRuntime> get_voice_output_runtime(
    const ApiKeyStore& api_keys,
    bool voice_enabled);

[[nodiscard]] std::vector<cha::web::ApiKeyDetail> list_api_keys(
    const ApiKeyStore& api_keys);
[[nodiscard]] cha::web::ApiKeyDetail create_api_key(
    ApiKeyStore& api_keys,
    const cha::web::CreateApiKeyRequest& create);
[[nodiscard]] cha::web::ApiKeyDetail rename_api_key(
    ApiKeyStore& api_keys,
    std::string_view id,
    std::string_view display_name);
[[nodiscard]] cha::web::ApiKeyDetail replace_api_key_value(
    ApiKeyStore& api_keys,
    std::string_view id,
    std::string_view value);
void delete_api_key(ApiKeyStore& api_keys, std::string_view id);

[[nodiscard]] std::optional<cha::web::R2StorageDetail> get_r2_storage(
    const ApiKeyStore& api_keys);
[[nodiscard]] cha::web::R2StorageDetail save_r2_storage(
    ApiKeyStore& api_keys,
    const cha::web::SaveR2StorageRequest& request);
void delete_r2_storage(ApiKeyStore& api_keys);

[[nodiscard]] cha::web::OpenAiAuth openai_auth_status(const OpenAiOAuth& owner);
[[nodiscard]] cha::web::OpenAiAuth start_openai_auth(
    OpenAiOAuth& owner,
    const std::atomic_bool& cancelled);
[[nodiscard]] cha::web::OpenAiAuth poll_openai_auth(
    OpenAiOAuth& owner,
    const std::atomic_bool& cancelled);
[[nodiscard]] cha::web::OpenAiAuth disconnect_openai_auth(OpenAiOAuth& owner);

} // namespace cha::app::settings
