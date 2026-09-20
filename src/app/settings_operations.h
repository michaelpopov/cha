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
class LiveSessionManager;
}

namespace cha::app::settings {

[[nodiscard]] ProviderSummary provider_summary(
    const WorkspaceProvider& provider);
[[nodiscard]] ProviderDetail provider_detail(
    const WorkspaceProvider& provider,
    bool writable,
    std::vector<std::string> used_by,
    const ApiKeyStore& api_keys);
[[nodiscard]] StyleDetail style_detail(
    const WorkspaceStyle& style,
    bool writable,
    std::vector<std::string> used_by);
[[nodiscard]] VoiceDetail voice_detail(
    const WorkspaceVoice& voice,
    bool writable,
    std::vector<std::string> used_by);
[[nodiscard]] ApiKeyDetail api_key_detail(
    const ApiKeyInfo& key,
    const Workspace& workspace);
[[nodiscard]] R2StorageDetail r2_storage_detail(
    const R2StorageInfo& key);
[[nodiscard]] OpenAiAuth openai_auth_from(
    const OpenAiOAuthSnapshot& snapshot);

[[nodiscard]] std::vector<ProviderSummary> list_providers(
    const Workspace& workspace);
[[nodiscard]] ProviderDetail get_provider(
    const Workspace& workspace,
    std::string_view id,
    const ApiKeyStore& api_keys);
[[nodiscard]] ProviderDetail create_provider(
    WorkspaceConfigStore& store,
    const ApiKeyStore& api_keys,
    const CreateProviderRequest& create);
[[nodiscard]] ProviderDetail update_provider(
    WorkspaceConfigStore& store,
    LiveSessionManager& live_sessions,
    const ApiKeyStore& api_keys,
    std::string_view id,
    const nlohmann::json& body);
void delete_provider(WorkspaceConfigStore& store, std::string_view id);
void test_provider(
    const Workspace& workspace,
    std::string_view id,
    const nlohmann::json& body,
    OpenAiOAuth& openai_auth,
    ApiKeyStore& api_keys,
    const std::atomic_bool& cancellation);

[[nodiscard]] std::vector<StyleDetail> list_styles(
    const Workspace& workspace);
[[nodiscard]] StyleDetail create_style(
    WorkspaceConfigStore& store,
    std::string_view display_name);
[[nodiscard]] StyleDetail update_style(
    WorkspaceConfigStore& store,
    LiveSessionManager& live_sessions,
    std::string_view id,
    const StyleUpdate& update);
void delete_style(WorkspaceConfigStore& store, std::string_view id);

[[nodiscard]] std::vector<VoiceDetail> list_voices(
    const Workspace& workspace);
[[nodiscard]] VoiceDetail create_voice(
    WorkspaceConfigStore& store,
    const CreateVoiceRequest& create);
[[nodiscard]] VoiceDetail update_voice(
    WorkspaceConfigStore& store,
    LiveSessionManager& live_sessions,
    std::string_view id,
    const VoiceUpdate& update);
void delete_voice(WorkspaceConfigStore& store, std::string_view id);

[[nodiscard]] std::optional<VoiceInputSettings> get_voice_input_settings(
    const Workspace& workspace);
[[nodiscard]] VoiceInputSettings save_voice_input_settings(
    WorkspaceConfigStore& store,
    const ApiKeyStore& api_keys,
    const VoiceInputSettings& update);
[[nodiscard]] std::optional<VoiceInputRuntime> get_voice_input_runtime(
    const Workspace& workspace,
    const ApiKeyStore& api_keys,
    bool voice_enabled);
[[nodiscard]] std::optional<std::string> voice_input_secret(
    const Workspace& workspace,
    const ApiKeyStore& api_keys,
    bool voice_enabled);

[[nodiscard]] std::optional<VoiceOutputSettings> get_voice_output_settings(
    const Workspace& workspace);
[[nodiscard]] VoiceOutputSettings save_voice_output_settings(
    WorkspaceConfigStore& store,
    const ApiKeyStore& api_keys,
    const VoiceOutputSettings& update);
[[nodiscard]] std::optional<VoiceOutputRuntime> get_voice_output_runtime(
    const Workspace& workspace,
    const ApiKeyStore& api_keys,
    bool voice_enabled);

[[nodiscard]] std::vector<ApiKeyDetail> list_api_keys(
    const Workspace& workspace,
    const ApiKeyStore& api_keys);
[[nodiscard]] ApiKeyDetail create_api_key(
    const Workspace& workspace,
    ApiKeyStore& api_keys,
    const CreateApiKeyRequest& create);
[[nodiscard]] ApiKeyDetail rename_api_key(
    const Workspace& workspace,
    ApiKeyStore& api_keys,
    std::string_view id,
    std::string_view display_name);
[[nodiscard]] ApiKeyDetail replace_api_key_value(
    const Workspace& workspace,
    ApiKeyStore& api_keys,
    std::string_view id,
    std::string_view value);
void delete_api_key(ApiKeyStore& api_keys, std::string_view id);

[[nodiscard]] std::optional<R2StorageDetail> get_r2_storage(
    const ApiKeyStore& api_keys);
[[nodiscard]] R2StorageDetail save_r2_storage(
    ApiKeyStore& api_keys,
    const SaveR2StorageRequest& request);
void delete_r2_storage(ApiKeyStore& api_keys);

[[nodiscard]] OpenAiAuth openai_auth_status(const OpenAiOAuth& owner);
[[nodiscard]] OpenAiAuth start_openai_auth(
    OpenAiOAuth& owner,
    const std::atomic_bool& cancelled);
[[nodiscard]] OpenAiAuth poll_openai_auth(
    OpenAiOAuth& owner,
    const std::atomic_bool& cancelled);
[[nodiscard]] OpenAiAuth disconnect_openai_auth(OpenAiOAuth& owner);

} // namespace cha::app::settings
