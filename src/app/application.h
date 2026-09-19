#pragma once

#include "app/media_resources.h"
#include "web/application_config.h"
#include "web/audio_download.h"
#include "web/command_queue.h"
#include "web/fish_audio.h"
#include "web/live_session_manager.h"
#include "web/protocol.h"
#include "web/r2_database_transfer.h"
#include "web/web_settings.h"
#include "workspace/workspace_config_store.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace cha {
class SessionRepository;
class Providers;
class ApiKeyStore;
class OpenAiOAuth;
}

namespace cha::web {
class CurrentVault;
class SessionMirror;

struct VaultCreate {
    std::string display_name;
    std::optional<std::string> copy_from;
    std::string password;
};

struct VaultUpdate {
    std::string display_name;
    std::string password;
};

struct VaultRegistrySnapshot {
    std::vector<VaultDefinition> vaults;
    VaultDefinition active;
};
}

namespace cha::app {

enum class ApplicationState {
    running,
    maintenance,
    stopping,
    unavailable,
};

struct ApplicationBootstrap {
    ApplicationState state{ApplicationState::running};
    std::uint64_t context_epoch{1};
    cha::web::Bootstrap presentation;
};

struct MaintenanceResult {
    ApplicationState state{ApplicationState::running};
    std::uint64_t context_epoch{1};
};

struct ApplicationCapabilities {
    bool can_modify{};
    bool can_transfer_r2{};
};

struct ResourceHooks {
    std::function<void(bool cancel)> pause;
    std::function<void()> resume;
};

using ContextChanged = std::function<
    void(std::uint64_t context_epoch, ApplicationState state)>;

class ApplicationError : public std::runtime_error {
public:
    explicit ApplicationError(
        cha::web::ErrorCode code,
        std::string message = {});
    cha::web::ErrorCode code;
};

// Terminal result for provider tests and OAuth network work. Abandoned replies
// ignore a later completion; the work itself is not rolled back.
class OperationReply {
public:
    struct Failure {
        cha::web::ErrorCode code{cha::web::ErrorCode::internal_error};
        std::string message;
    };
    using Result = std::variant<nlohmann::json, Failure>;

    bool complete(nlohmann::json result);
    bool fail(cha::web::ErrorCode code, std::string message);
    void set_ready_callback(std::function<void()> callback);
    [[nodiscard]] std::optional<Result> peek() const;
    void abandon() const;

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable ready_;
    mutable bool abandoned_{};
    std::optional<Result> result_;
    mutable std::function<void()> ready_callback_;
};

[[nodiscard]] std::string_view application_state_name(
    ApplicationState state) noexcept;

cha::web::WebSettings native_settings();

// Transport-neutral composition root. Construction does not bind a port or
// start a listener. The temporary HTTP runtime wraps this object.
class Application {
public:
    static std::unique_ptr<Application> open(
        const cha::web::ApplicationCommand& command,
        std::string vault_password = {},
        cha::web::WebSettings settings = native_settings());

    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    [[nodiscard]] ApplicationBootstrap bootstrap();
    [[nodiscard]] cha::web::CreateSessionSuccess create_session(
        std::string_view forum_id,
        std::string label,
        std::uint64_t epoch = 0);
    [[nodiscard]] std::variant<
        cha::web::OpenSessionSuccess,
        cha::web::ErrorCode>
    open_session(
        std::string_view forum_id,
        std::string_view session_id,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::CommandSubmitResult submit(
        std::string_view forum_id,
        std::string_view session_id,
        cha::web::WebCommand command,
        std::uint64_t epoch = 0);
    [[nodiscard]] std::variant<
        std::shared_ptr<cha::web::CommandReply>,
        cha::web::ErrorCode>
    submit_async(
        std::string_view forum_id,
        std::string_view session_id,
        cha::web::WebCommand command,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::CommandSubmitResult stop(
        std::string_view forum_id,
        std::string_view session_id,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::CommandSubmitResult snapshot(
        std::string_view forum_id,
        std::string_view session_id,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::CommandSubmitResult subscribe(
        std::string_view forum_id,
        std::string_view session_id,
        cha::web::SubscribeCommand command,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::CommandSubmitResult unsubscribe(
        std::string_view forum_id,
        std::string_view session_id,
        cha::web::UnsubscribeCommand command,
        std::uint64_t epoch = 0);
    void close_session(
        std::string_view forum_id,
        std::string_view session_id,
        std::uint64_t epoch = 0);
    [[nodiscard]] std::optional<cha::web::ErrorCode> delete_session(
        std::string_view forum_id,
        std::string_view session_id,
        std::uint64_t epoch = 0);
    [[nodiscard]] std::vector<cha::web::SessionListing> list_sessions(
        std::string_view forum_id,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::SessionLabelResult rename_session(
        std::string_view forum_id,
        std::string_view session_id,
        std::string label,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::SessionExport export_session(
        std::string_view forum_id,
        std::string_view session_id,
        std::uint64_t epoch = 0);

    [[nodiscard]] cha::web::CharacterDetail get_character(
        std::string_view character_id,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::CharacterDetail create_character(
        cha::web::CreateCharacterRequest create,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::CharacterDetail update_character(
        std::string_view character_id,
        cha::web::CharacterSettingsUpdate update,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::CharacterDetail update_character_definition(
        std::string_view character_id,
        cha::web::CharacterDefinitionUpdate update,
        std::uint64_t epoch = 0);
    void delete_character(
        std::string_view character_id,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::MarkdownFile get_character_file(
        std::string_view character_id,
        std::string_view filename,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::MarkdownFile create_character_file(
        std::string_view character_id,
        std::string filename,
        std::string content,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::MarkdownFile update_character_file(
        std::string_view character_id,
        std::string filename,
        std::string content,
        std::uint64_t epoch = 0);
    void delete_character_file(
        std::string_view character_id,
        std::string_view filename,
        std::uint64_t epoch = 0);

    [[nodiscard]] cha::web::PersonaDetail get_persona(
        std::string_view persona_id,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::PersonaDetail create_persona(
        std::string display_name,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::PersonaDetail update_persona(
        std::string_view persona_id,
        cha::web::PersonaUpdate update,
        std::uint64_t epoch = 0);
    void delete_persona(
        std::string_view persona_id,
        std::uint64_t epoch = 0);

    [[nodiscard]] cha::web::ForumDetail get_forum(
        std::string_view forum_id,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::ForumDetail create_forum(
        cha::web::CreateForumRequest create,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::ForumDetail update_forum(
        std::string_view forum_id,
        cha::web::ForumUpdate update,
        std::uint64_t epoch = 0);
    void delete_forum(
        std::string_view forum_id,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::ForumDetail update_forum_members(
        std::string_view forum_id,
        cha::web::ForumMembersUpdate update,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::MarkdownFile get_forum_file(
        std::string_view forum_id,
        std::string_view filename,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::MarkdownFile create_forum_file(
        std::string_view forum_id,
        std::string filename,
        std::string content,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::MarkdownFile update_forum_file(
        std::string_view forum_id,
        std::string filename,
        std::string content,
        std::uint64_t epoch = 0);
    void delete_forum_file(
        std::string_view forum_id,
        std::string_view filename,
        std::uint64_t epoch = 0);

    [[nodiscard]] std::vector<cha::web::ProviderSummary> list_providers(
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::ProviderDetail get_provider(
        std::string_view provider_id,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::ProviderDetail create_provider(
        cha::web::CreateProviderRequest create,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::ProviderDetail update_provider(
        std::string_view provider_id,
        nlohmann::json body,
        std::uint64_t epoch = 0);
    void delete_provider(
        std::string_view provider_id,
        std::uint64_t epoch = 0);
    [[nodiscard]] std::shared_ptr<OperationReply> test_provider(
        std::string_view provider_id,
        nlohmann::json body,
        std::uint64_t epoch = 0);

    [[nodiscard]] std::vector<cha::web::StyleDetail> list_styles(
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::StyleDetail create_style(
        std::string display_name,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::StyleDetail update_style(
        std::string_view style_id,
        cha::web::StyleUpdate update,
        std::uint64_t epoch = 0);
    void delete_style(std::string_view style_id, std::uint64_t epoch = 0);

    [[nodiscard]] std::vector<cha::web::VoiceDetail> list_voices(
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::VoiceDetail create_voice(
        cha::web::CreateVoiceRequest create,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::VoiceDetail update_voice(
        std::string_view voice_id,
        cha::web::VoiceUpdate update,
        std::uint64_t epoch = 0);
    void delete_voice(std::string_view voice_id, std::uint64_t epoch = 0);

    [[nodiscard]] std::optional<cha::web::VoiceInputSettings>
    get_voice_input_settings(std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::VoiceInputSettings save_voice_input_settings(
        cha::web::VoiceInputSettings settings,
        std::uint64_t epoch = 0);
    [[nodiscard]] std::optional<cha::web::VoiceInputRuntime>
    get_voice_input_runtime(std::uint64_t epoch = 0);
    [[nodiscard]] std::optional<cha::web::VoiceOutputSettings>
    get_voice_output_settings(std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::VoiceOutputSettings save_voice_output_settings(
        cha::web::VoiceOutputSettings settings,
        std::uint64_t epoch = 0);
    [[nodiscard]] std::optional<cha::web::VoiceOutputRuntime>
    get_voice_output_runtime(std::uint64_t epoch = 0);

    [[nodiscard]] std::shared_ptr<OperationReply> start_speech(
        std::string_view connection_id,
        std::uint64_t request_id,
        std::string text,
        cha::web::FishAudioSynthesis synthesis,
        std::uint64_t epoch = 0);
    void cancel_speech(
        std::string_view connection_id,
        std::uint64_t request_id,
        std::uint64_t epoch = 0);
    void release_resource(
        std::string_view connection_id,
        std::string_view resource_id,
        std::uint64_t epoch = 0);

    [[nodiscard]] cha::web::AudioAcceptance start_audio(
        std::string_view forum_id,
        std::string_view session_id,
        EntryId entry_id,
        cha::web::AudioDownloadRequest request,
        std::uint64_t epoch = 0);
    [[nodiscard]] std::vector<cha::web::AudioAcceptance> start_audio_batch(
        std::string_view forum_id,
        std::string_view session_id,
        cha::web::AudioDownloadBatchRequest request,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::AudioDownloadStatus audio_status(
        std::string_view forum_id,
        std::string_view session_id,
        std::string_view vault_name,
        std::uint64_t epoch = 0);
    [[nodiscard]] MediaResource audio_source(
        std::string_view connection_id,
        std::string_view forum_id,
        std::string_view session_id,
        EntryId entry_id,
        std::string_view vault_name,
        std::uint64_t epoch = 0);
    void clear_audio_cache(
        std::string_view forum_id,
        std::string_view session_id,
        std::uint64_t epoch = 0);

    [[nodiscard]] std::shared_ptr<OperationReply> connect_voice_input(
        std::string_view connection_id,
        std::uint64_t request_id,
        std::string sdp,
        std::vector<std::string> languages,
        std::uint64_t epoch = 0);
    void cancel_voice_input(
        std::string_view connection_id,
        std::uint64_t request_id,
        std::uint64_t epoch = 0);

    [[nodiscard]] std::optional<ResourceBytes> read_resource(
        std::string_view connection_id,
        std::string_view resource_id) const;
    void release_connection_resources(std::string_view connection_id);
    void set_speech_url_override(std::string url);

    [[nodiscard]] std::vector<cha::web::ApiKeyDetail> list_api_keys(
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::ApiKeyDetail create_api_key(
        cha::web::CreateApiKeyRequest create,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::ApiKeyDetail rename_api_key(
        std::string_view api_key_id,
        std::string display_name,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::ApiKeyDetail replace_api_key_value(
        std::string_view api_key_id,
        std::string value,
        std::uint64_t epoch = 0);
    void delete_api_key(std::string_view api_key_id, std::uint64_t epoch = 0);

    [[nodiscard]] std::optional<cha::web::R2StorageDetail> get_r2_storage(
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::R2StorageDetail save_r2_storage(
        cha::web::SaveR2StorageRequest request,
        std::uint64_t epoch = 0);
    void delete_r2_storage(std::uint64_t epoch = 0);

    [[nodiscard]] cha::web::OpenAiAuth openai_auth_status(
        std::uint64_t epoch = 0);
    [[nodiscard]] std::shared_ptr<OperationReply> start_openai_auth(
        std::uint64_t epoch = 0);
    [[nodiscard]] std::shared_ptr<OperationReply> poll_openai_auth(
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::OpenAiAuth disconnect_openai_auth(
        std::uint64_t epoch = 0);

    [[nodiscard]] cha::web::VaultRegistrySnapshot vault_snapshot() const;
    [[nodiscard]] cha::web::VaultDefinition create_vault(
        cha::web::VaultCreate create,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::VaultDefinition update_vault(
        std::string_view current_name,
        cha::web::VaultUpdate update,
        std::uint64_t epoch = 0);
    void delete_vault(std::string_view name, std::uint64_t epoch = 0);
    [[nodiscard]] MaintenanceResult switch_vault(
        std::string_view name,
        std::string password = {},
        std::uint64_t epoch = 0);
    [[nodiscard]] MaintenanceResult merge_vault(
        std::string_view source_name,
        std::string password = {},
        std::uint64_t epoch = 0);
    [[nodiscard]] std::vector<std::string> list_r2_vaults(
        std::uint64_t epoch = 0) const;
    [[nodiscard]] cha::web::VaultDefinition download_r2_vault(
        std::string_view name,
        std::uint64_t epoch = 0);
    [[nodiscard]] cha::web::R2DatabaseTransfer upload_database();
    [[nodiscard]] cha::web::R2DatabaseTransfer download_database();
    [[nodiscard]] WorkspaceConfigTransfer import_configuration();
    [[nodiscard]] WorkspaceConfigTransfer export_configuration();
    void save_file(
        std::uint64_t epoch,
        const std::filesystem::path& destination,
        std::string_view contents);

    [[nodiscard]] std::optional<FullSessionId> selected_session() const;
    [[nodiscard]] std::uint64_t context_epoch() const;
    // Shares exclusion with maintenance: epoch check and resource use take
    // the same lifecycle mutex. A matching name is not enough.
    [[nodiscard]] std::optional<cha::web::ErrorCode> check_context(
        std::uint64_t epoch) const;
    [[nodiscard]] bool running() const;
    [[nodiscard]] ApplicationState state() const;
    [[nodiscard]] ApplicationCapabilities capabilities() const;
    [[nodiscard]] bool has_r2_storage() const;
    void set_resource_hooks(ResourceHooks hooks);
    void set_context_changed(ContextChanged callback);

    void request_shutdown();
    [[nodiscard]] bool join_shutdown(
        std::chrono::milliseconds grace = std::chrono::milliseconds{10000});

    [[nodiscard]] cha::web::ApplicationCommand& command();
    [[nodiscard]] const cha::web::ApplicationCommand& command() const;
    [[nodiscard]] const cha::web::WebSettings& settings() const;
    [[nodiscard]] cha::web::CurrentVault& current_vault();
    [[nodiscard]] const cha::web::CurrentVault& current_vault() const;
    [[nodiscard]] cha::WorkspaceConfigStore& store();
    [[nodiscard]] std::shared_ptr<cha::SessionRepository> sessions();
    [[nodiscard]] cha::web::LiveSessionManager& live_sessions();
    [[nodiscard]] cha::web::AudioDownloadManager& audio_downloads();
    [[nodiscard]] cha::web::FishAudioProxy& speech_proxy();
    [[nodiscard]] cha::Providers& providers();
    [[nodiscard]] cha::ApiKeyStore& api_keys();
    [[nodiscard]] cha::OpenAiOAuth& openai_auth();
    [[nodiscard]] std::shared_ptr<cha::web::SessionMirror> mirror();
    [[nodiscard]] std::string active_password() const;
    std::string& mutable_active_password();
    void set_active_password(std::string password);
    [[nodiscard]] std::timed_mutex& lifecycle_mutex();
    [[nodiscard]] bool unusable() const;
    bool& mutable_unusable();
    void mark_unusable();
    [[nodiscard]] bool stopping() const;

private:
    struct Impl;
    explicit Application(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace cha::app
