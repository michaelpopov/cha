#pragma once

#include "media/media_resources.h"
#include "media/xai_socket.h"
#include "runtime/runtime_settings.h"
#include "app/application_config.h"
#include "media/audio_download.h"
#include "runtime/command_queue.h"
#include "providers/fish_audio.h"
#include "runtime/live_session_manager.h"
#include "runtime/protocol.h"
#include "app/r2_database_transfer.h"
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
class ApiKeyStore;
class CurrentVault;

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

struct ApplicationCapabilities {
    bool can_modify{};
    bool can_transfer_r2{};
};

struct ApplicationBootstrap {
    ApplicationState state{ApplicationState::running};
    std::uint64_t context_epoch{1};
    ApplicationCapabilities capabilities;
    Bootstrap presentation;
};

struct MaintenanceResult {
    ApplicationState state{ApplicationState::running};
    std::uint64_t context_epoch{1};
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
        ErrorCode code,
        std::string message = {});
    ErrorCode code;
};

// Terminal result for provider tests and OAuth network work. Abandoned replies
// ignore a later completion; the work itself is not rolled back.
class OperationReply {
public:
    struct Failure {
        ErrorCode code{ErrorCode::internal_error};
        std::string message;
    };
    using Result = std::variant<nlohmann::json, Failure>;

    bool complete(nlohmann::json result);
    bool fail(ErrorCode code, std::string message);
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

// Composition root. Construction does not bind a port or start a listener.
class Application {
public:
    static std::unique_ptr<Application> open(
        const ApplicationCommand& command,
        std::string vault_password = {},
        RuntimeSettings settings = {});

    ~Application();
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    [[nodiscard]] ApplicationBootstrap bootstrap();
    // Context-bound operations require the nonzero epoch captured by the caller.
    [[nodiscard]] CreateSessionSuccess create_session(
        std::string_view forum_id,
        std::string label,
        std::uint64_t epoch);
    [[nodiscard]] std::variant<
        OpenSessionSuccess,
        ErrorCode>
    open_session(
        std::string_view forum_id,
        std::string_view session_id,
        std::uint64_t epoch);
    [[nodiscard]] CommandSubmitResult submit(
        std::string_view forum_id,
        std::string_view session_id,
        WebCommand command,
        std::uint64_t epoch);
    [[nodiscard]] std::variant<
        std::shared_ptr<CommandReply>,
        ErrorCode>
    submit_async(
        std::string_view forum_id,
        std::string_view session_id,
        WebCommand command,
        std::uint64_t epoch,
        std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::time_point::max());
    [[nodiscard]] CommandSubmitResult stop(
        std::string_view forum_id,
        std::string_view session_id,
        std::uint64_t epoch);
    [[nodiscard]] CommandSubmitResult snapshot(
        std::string_view forum_id,
        std::string_view session_id,
        std::uint64_t epoch);
    [[nodiscard]] CommandSubmitResult subscribe(
        std::string_view forum_id,
        std::string_view session_id,
        SubscribeCommand command,
        std::uint64_t epoch);
    [[nodiscard]] CommandSubmitResult unsubscribe(
        std::string_view forum_id,
        std::string_view session_id,
        UnsubscribeCommand command,
        std::uint64_t epoch);
    void close_session(
        std::string_view forum_id,
        std::string_view session_id,
        std::uint64_t epoch);
    [[nodiscard]] std::optional<ErrorCode> delete_session(
        std::string_view forum_id,
        std::string_view session_id,
        std::uint64_t epoch);
    [[nodiscard]] std::vector<SessionListing> list_sessions(
        std::string_view forum_id,
        std::uint64_t epoch);
    [[nodiscard]] SessionLabelResult rename_session(
        std::string_view forum_id,
        std::string_view session_id,
        std::string label,
        std::uint64_t epoch);
    [[nodiscard]] SessionExport export_session(
        std::string_view forum_id,
        std::string_view session_id,
        std::uint64_t epoch);

    [[nodiscard]] CharacterDetail get_character(
        std::string_view character_id,
        std::uint64_t epoch);
    [[nodiscard]] CharacterDetail create_character(
        CreateCharacterRequest create,
        std::uint64_t epoch);
    [[nodiscard]] CharacterDetail update_character(
        std::string_view character_id,
        CharacterSettingsUpdate update,
        std::uint64_t epoch);
    [[nodiscard]] CharacterDetail update_character_definition(
        std::string_view character_id,
        CharacterDefinitionUpdate update,
        std::uint64_t epoch);
    void delete_character(
        std::string_view character_id,
        std::uint64_t epoch);
    [[nodiscard]] MarkdownFile get_character_file(
        std::string_view character_id,
        std::string_view filename,
        std::uint64_t epoch);
    [[nodiscard]] MarkdownFile create_character_file(
        std::string_view character_id,
        std::string filename,
        std::string content,
        std::uint64_t epoch);
    [[nodiscard]] MarkdownFile update_character_file(
        std::string_view character_id,
        std::string filename,
        std::string content,
        std::uint64_t epoch);
    void delete_character_file(
        std::string_view character_id,
        std::string_view filename,
        std::uint64_t epoch);

    [[nodiscard]] PersonaDetail get_persona(
        std::string_view persona_id,
        std::uint64_t epoch);
    [[nodiscard]] PersonaDetail create_persona(
        std::string display_name,
        std::uint64_t epoch);
    [[nodiscard]] PersonaDetail update_persona(
        std::string_view persona_id,
        PersonaUpdate update,
        std::uint64_t epoch);
    void delete_persona(
        std::string_view persona_id,
        std::uint64_t epoch);

    [[nodiscard]] ForumDetail get_forum(
        std::string_view forum_id,
        std::uint64_t epoch);
    [[nodiscard]] ForumDetail create_forum(
        CreateForumRequest create,
        std::uint64_t epoch);
    [[nodiscard]] ForumDetail update_forum(
        std::string_view forum_id,
        ForumUpdate update,
        std::uint64_t epoch);
    void delete_forum(
        std::string_view forum_id,
        std::uint64_t epoch);
    [[nodiscard]] ForumDetail update_forum_members(
        std::string_view forum_id,
        ForumMembersUpdate update,
        std::uint64_t epoch);
    [[nodiscard]] MarkdownFile get_forum_file(
        std::string_view forum_id,
        std::string_view filename,
        std::uint64_t epoch);
    [[nodiscard]] MarkdownFile create_forum_file(
        std::string_view forum_id,
        std::string filename,
        std::string content,
        std::uint64_t epoch);
    [[nodiscard]] MarkdownFile update_forum_file(
        std::string_view forum_id,
        std::string filename,
        std::string content,
        std::uint64_t epoch);
    void delete_forum_file(
        std::string_view forum_id,
        std::string_view filename,
        std::uint64_t epoch);

    [[nodiscard]] std::vector<ProviderSummary> list_providers(
        std::uint64_t epoch);
    [[nodiscard]] ProviderDetail get_provider(
        std::string_view provider_id,
        std::uint64_t epoch);
    [[nodiscard]] ProviderDetail create_provider(
        CreateProviderRequest create,
        std::uint64_t epoch);
    [[nodiscard]] ProviderDetail update_provider(
        std::string_view provider_id,
        nlohmann::json body,
        std::uint64_t epoch);
    void delete_provider(
        std::string_view provider_id,
        std::uint64_t epoch);
    [[nodiscard]] std::shared_ptr<OperationReply> test_provider(
        std::string_view provider_id,
        nlohmann::json body,
        std::uint64_t epoch);

    [[nodiscard]] std::vector<StyleDetail> list_styles(
        std::uint64_t epoch);
    [[nodiscard]] StyleDetail create_style(
        std::string display_name,
        std::uint64_t epoch);
    [[nodiscard]] StyleDetail update_style(
        std::string_view style_id,
        StyleUpdate update,
        std::uint64_t epoch);
    void delete_style(std::string_view style_id, std::uint64_t epoch);

    [[nodiscard]] std::vector<VoiceDetail> list_voices(
        std::uint64_t epoch);
    [[nodiscard]] VoiceDetail create_voice(
        CreateVoiceRequest create,
        std::uint64_t epoch);
    [[nodiscard]] VoiceDetail update_voice(
        std::string_view voice_id,
        VoiceUpdate update,
        std::uint64_t epoch);
    void delete_voice(std::string_view voice_id, std::uint64_t epoch);

    [[nodiscard]] std::optional<JevSettings> get_jev_settings(std::uint64_t epoch);
    [[nodiscard]] JevSettings save_jev_settings(JevSettings settings, std::uint64_t epoch);
    void disable_jev(std::uint64_t epoch);
    [[nodiscard]] std::optional<VoiceInputSettings>
    get_voice_input_settings(std::uint64_t epoch);
    [[nodiscard]] VoiceInputSettings save_voice_input_settings(
        VoiceInputSettings settings,
        std::uint64_t epoch);
    [[nodiscard]] std::optional<VoiceInputRuntime>
    get_voice_input_runtime(std::uint64_t epoch);
    [[nodiscard]] std::optional<VoiceOutputSettings>
    get_voice_output_settings(std::uint64_t epoch);
    [[nodiscard]] VoiceOutputSettings save_voice_output_settings(
        VoiceOutputSettings settings,
        std::uint64_t epoch);
    [[nodiscard]] std::optional<VoiceOutputRuntime>
    get_voice_output_runtime(std::uint64_t epoch);

    [[nodiscard]] std::shared_ptr<OperationReply> start_speech(
        std::string_view connection_id,
        std::uint64_t request_id,
        std::string text,
        FishAudioSynthesis synthesis,
        std::uint64_t epoch);
    void cancel_speech(
        std::string_view connection_id,
        std::uint64_t request_id,
        std::uint64_t epoch);
    void release_resource(
        std::string_view connection_id,
        std::string_view resource_id,
        std::uint64_t epoch);

    [[nodiscard]] AudioAcceptance start_audio(
        std::string_view forum_id,
        std::string_view session_id,
        EntryId entry_id,
        AudioDownloadRequest request,
        std::uint64_t epoch);
    [[nodiscard]] std::vector<AudioAcceptance> start_audio_batch(
        std::string_view forum_id,
        std::string_view session_id,
        AudioDownloadBatchRequest request,
        std::uint64_t epoch);
    [[nodiscard]] AudioDownloadStatus audio_status(
        std::string_view forum_id,
        std::string_view session_id,
        std::string_view vault_name,
        std::uint64_t epoch);
    [[nodiscard]] MediaResource audio_source(
        std::string_view connection_id,
        std::string_view forum_id,
        std::string_view session_id,
        EntryId entry_id,
        std::string_view vault_name,
        std::uint64_t epoch);
    void clear_audio_cache(
        std::string_view forum_id,
        std::string_view session_id,
        std::uint64_t epoch);

    [[nodiscard]] std::shared_ptr<OperationReply> connect_voice_input(
        std::string_view connection_id,
        std::uint64_t request_id,
        std::string sdp,
        std::vector<std::string> languages,
        std::uint64_t epoch);
    void cancel_voice_input(
        std::string_view connection_id,
        std::uint64_t request_id,
        std::uint64_t epoch);
    [[nodiscard]] std::shared_ptr<OperationReply> start_xai_voice_input(
        std::string connection_id,
        std::uint64_t request_id,
        std::string session_id,
        std::vector<std::string> languages,
        std::uint64_t epoch,
        std::chrono::milliseconds deadline);
    [[nodiscard]] std::shared_ptr<OperationReply> send_xai_voice_audio(
        std::string connection_id,
        std::uint64_t request_id,
        std::string session_id,
        std::string pcm_base64,
        std::uint64_t epoch,
        std::chrono::milliseconds deadline);
    [[nodiscard]] std::shared_ptr<OperationReply> stop_xai_voice_input(
        std::string connection_id,
        std::uint64_t request_id,
        std::string session_id,
        std::int64_t remaining_ms,
        std::uint64_t epoch,
        std::chrono::milliseconds deadline);
    void cancel_xai_voice_input(
        std::string_view connection_id,
        std::string_view session_id,
        std::uint64_t epoch);
    void expire_xai_voice_request(
        std::string_view connection_id,
        std::uint64_t request_id);
    void set_xai_socket_factory_for_tests(
        std::function<std::unique_ptr<media::XaiSocket>()> factory);

    [[nodiscard]] std::optional<ResourceBytes> read_resource(
        std::string_view connection_id,
        std::string_view resource_id) const;
    [[nodiscard]] std::optional<AudioChunk> read_resource_chunk(
        std::string_view connection_id, std::string_view resource_id, std::uint64_t offset) const;
    // Host cleanup after a request or connection is abandoned; valid in any state.
    void release_request_resources(
        std::string_view connection_id,
        std::uint64_t request_id);
    void release_connection_resources(std::string_view connection_id);
    void set_speech_url_override(std::string url);

    [[nodiscard]] std::vector<ApiKeyDetail> list_api_keys(
        std::uint64_t epoch);
    [[nodiscard]] ApiKeyDetail create_api_key(
        CreateApiKeyRequest create,
        std::uint64_t epoch);
    [[nodiscard]] ApiKeyDetail rename_api_key(
        std::string_view api_key_id,
        std::string display_name,
        std::uint64_t epoch);
    [[nodiscard]] ApiKeyDetail replace_api_key_value(
        std::string_view api_key_id,
        std::string value,
        std::uint64_t epoch);
    void delete_api_key(std::string_view api_key_id, std::uint64_t epoch);

    [[nodiscard]] std::optional<R2StorageDetail> get_r2_storage(
        std::uint64_t epoch);
    [[nodiscard]] R2StorageDetail save_r2_storage(
        SaveR2StorageRequest request,
        std::uint64_t epoch);
    void delete_r2_storage(std::uint64_t epoch);

    [[nodiscard]] OpenAiAuth openai_auth_status(
        std::uint64_t epoch);
    [[nodiscard]] std::shared_ptr<OperationReply> start_openai_auth(
        std::uint64_t epoch);
    [[nodiscard]] std::shared_ptr<OperationReply> poll_openai_auth(
        std::uint64_t epoch);
    [[nodiscard]] OpenAiAuth disconnect_openai_auth(
        std::uint64_t epoch);

    [[nodiscard]] VaultRegistrySnapshot vault_snapshot() const;
    [[nodiscard]] VaultDefinition create_vault(
        VaultCreate create,
        std::uint64_t epoch);
    [[nodiscard]] VaultDefinition update_vault(
        std::string_view current_name,
        VaultUpdate update,
        std::uint64_t epoch);
    void delete_vault(std::string_view name, std::uint64_t epoch);
    [[nodiscard]] MaintenanceResult switch_vault(
        std::string_view name,
        std::string password,
        std::uint64_t epoch);
    [[nodiscard]] MaintenanceResult merge_vault(
        std::string_view source_name,
        std::string password,
        std::uint64_t epoch);
    [[nodiscard]] std::vector<std::string> list_r2_vaults(
        std::uint64_t epoch) const;
    [[nodiscard]] VaultDefinition download_r2_vault(
        std::string_view name,
        std::uint64_t epoch);
    [[nodiscard]] R2UploadCheck check_database_upload(
        std::uint64_t epoch) const;
    [[nodiscard]] R2DatabaseTransfer upload_database(
        std::optional<std::string> expected_etag,
        std::uint64_t epoch);
    [[nodiscard]] R2DatabaseTransfer download_database(std::uint64_t epoch);
    [[nodiscard]] WorkspaceConfigTransfer import_configuration(std::uint64_t epoch);
    [[nodiscard]] WorkspaceConfigTransfer export_configuration(std::uint64_t epoch);
    void save_file(
        std::uint64_t epoch,
        const std::filesystem::path& destination,
        std::string_view contents);

    [[nodiscard]] std::optional<FullSessionId> selected_session() const;
    [[nodiscard]] std::uint64_t context_epoch() const;
    // Nonblocking preflight only; it does not reserve the context. Operations
    // needing stable vault state must also check admission under their owning lock.
    [[nodiscard]] std::optional<ErrorCode> check_context(
        std::uint64_t epoch) const;
    [[nodiscard]] bool running() const;
    [[nodiscard]] ApplicationState state() const;
    [[nodiscard]] ApplicationCapabilities capabilities() const;
    [[nodiscard]] bool has_r2_storage() const;
    void set_resource_hooks(ResourceHooks hooks);
    void set_context_changed(ContextChanged callback);

    void request_shutdown();
    // Native joins wait for an in-flight maintenance section before starting
    // their bounded shutdown grace period.
    void wait_for_maintenance() const;
    [[nodiscard]] bool join_shutdown(
        std::chrono::milliseconds grace = std::chrono::milliseconds{10000});

    [[nodiscard]] const RuntimeSettings& settings() const;
    // Subscription completion and teardown must work with an expired context.
    // Uses only the manager lock: actor callbacks must never wait on the
    // application lifecycle lock while maintenance is draining those actors.
    // May return empty during maintenance/shutdown. Retain the handle while used.
    [[nodiscard]] LiveSessionHandle subscription_handle(
        std::string_view forum_id, std::string_view session_id);

    // Test seams — not part of the application boundary.
    [[nodiscard]] std::size_t live_session_count() const;
    [[nodiscard]] CurrentVault& current_vault();
    [[nodiscard]] const CurrentVault& current_vault() const;
    [[nodiscard]] WorkspaceConfigStore& store();
    [[nodiscard]] ApiKeyStore& api_keys();
    [[nodiscard]] std::string active_password() const;
    void mark_unusable();

private:
    struct Impl;
    explicit Application(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace cha::app
