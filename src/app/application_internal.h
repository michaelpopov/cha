#pragma once

// Private runtime shared by the application, vault, and media implementations.
#include "app/application.h"
#include "notes/notes_service.h"
#include "app/assistant_service.h"
#include "app/background_jobs.h"
#include "app/xai_voice_session.h"
#include "media/pending_media_registry.h"
#include "providers/api_key_store.h"
#include "providers/openai_oauth.h"
#include "providers/providers.h"
#include "storage/session_repository.h"
#include "app/d1_sync.h"
#include "app/current_vault.h"
#include "session/session_mirror.h"

#include <atomic>

namespace cha::app {

struct Application::Impl {
    explicit Impl(
        const ApplicationCommand& selected_command,
        std::string selected_vault_password,
        RuntimeSettings selected_settings,
        D1Client::Transport d1_transport);

    static constexpr unsigned can_modify = 1U << 0;

    void publish_capabilities_locked();

    [[nodiscard]] ApplicationCapabilities capabilities() const noexcept;

    [[nodiscard]] std::optional<ErrorCode> admit_locked(std::uint64_t epoch) const;

    void require_admitted(std::uint64_t epoch) const;

    void pause_resources(bool cancel);

    void resume_resources();

    struct PendingContextNotice {
        ContextChanged callback;
        std::uint64_t epoch{};
        ApplicationState state{ApplicationState::running};

        void dispatch() {
            if (!callback) return;
            ContextChanged fn = std::move(callback);
            callback = {};
            fn(epoch, state);
        }
    };

    // Private coordinator for vault changes and database recovery. It uses the
    // application's existing admission state; callbacks are dispatched only
    // after releasing the lifecycle lock.
    enum class SyncStart { resume, open, recover, credentials_saved };

    struct VaultMaintenance {
        explicit VaultMaintenance(Impl& application) : app(application) {}

        std::optional<CommandResult> submit_welcome_command(
            std::string_view input, std::uint64_t epoch);
        VaultRegistrySnapshot vault_snapshot() const;
        VaultDefinition create_vault(VaultCreate create, std::uint64_t epoch);
        VaultDefinition update_vault(
            std::string_view current_name,
            VaultUpdate update,
            std::uint64_t epoch);
        void delete_vault(std::string_view name, std::uint64_t epoch);
        std::vector<std::string> list_r2_vaults(std::uint64_t epoch) const;
        VaultDefinition download_r2_vault(
            std::string_view name,
            std::uint64_t epoch);
        MaintenanceResult switch_vault(
            std::string_view name,
            std::string password,
            std::uint64_t epoch);
        MaintenanceResult merge_vault(
            std::string_view source_name,
            std::string password,
            std::uint64_t epoch);
        R2UploadCheck check_database_upload(std::uint64_t epoch) const;
        std::shared_ptr<OperationReply> start_database_upload(
            std::optional<std::string> expected_etag,
            std::uint64_t epoch);
        R2DatabaseTransfer download_database(std::uint64_t epoch, bool from_welcome = false);
        WorkspaceConfigTransfer import_configuration(std::uint64_t epoch);
        WorkspaceConfigTransfer export_configuration(std::uint64_t epoch);

        void publish_vault_names();
        void delete_forum(std::string_view forum_id, std::uint64_t epoch);
    private:
        void run_welcome_command(bool download, bool confirmed, std::string target,
            std::uint64_t epoch);
        void post_welcome_notice(std::string text, std::string error_code = {});
        R2StorageKey require_r2_key() const;
        void clear_downloaded_inbox(
            const std::filesystem::path& database, std::string_view name);
        VaultDefinition save_r2_vault_locked(
            std::string_view name, const R2StorageKey& r2, bool from_welcome);
        void switch_vault_locked(const VaultDefinition& target, std::string password,
            PendingContextNotice& notice, bool from_welcome = false);
        void publish_vault(VaultDefinition vault);
        R2DatabaseUpload prepare_upload(
            std::uint64_t epoch, VaultDefinition& vault, R2StorageKey& storage);
        // Takes the upload, so the copy is removed before the result is
        // reported. Clears vault_upload_running when it returns or throws.
        R2DatabaseTransfer finish_upload(
            R2DatabaseUpload upload,
            const VaultDefinition& vault,
            const R2StorageKey& storage,
            std::string_view expected_etag,
            const std::function<bool()>& cancelled);
        std::chrono::milliseconds maintenance_grace() const;
        std::uint64_t publish_epoch(
            PendingContextNotice& notice,
            ApplicationState next_state,
            std::optional<SyncStart> sync_start = std::nullopt);
        // Caller holds lifecycle_mutex. False keeps the previous vault selected.
        bool drain_for_maintenance(
            PendingContextNotice& notice,
            bool cancel_audio = true, bool require_main = true);
        void cancel_maintenance_locked(PendingContextNotice& notice);
        void end_maintenance_locked(
            bool available, PendingContextNotice& notice,
            SyncStart sync_start = SyncStart::resume);
        void reopen(
            WorkspaceConfigStore::MaintenanceGuard& database,
            const SessionRepository::MaintenanceGuard& repository);
        void reopen_after_failure(
            WorkspaceConfigStore::MaintenanceGuard& database,
            const SessionRepository::MaintenanceGuard& repository);
        void rebuild_mirror(const std::optional<std::filesystem::path>& root);
        template<typename Operation>
        auto maintain_database(
            std::uint64_t epoch,
            Operation operation,
            bool cancel_audio = true,
            SyncStart sync_start = SyncStart::resume,
            const std::function<void()>& before_drain = {},
            const std::function<void()>& after_reopen = {});

        Impl& app;
        std::optional<LiveSessionGlobalMaintenance> global_maintenance;
    };

    void take_context_notice(PendingContextNotice& notice);

    ~Impl();

    ApplicationCommand command;
    std::string active_password;
    RuntimeSettings settings;
    CurrentVault current_vault_;
    std::unique_ptr<WorkspaceConfigStore> store;
    std::shared_ptr<SessionRepository> sessions;
    std::shared_ptr<SessionMirror> mirror;
    std::unique_ptr<ApiKeyStore> api_keys;
    std::unique_ptr<OpenAiOAuth> openai_auth;
    std::mutex web_search_url_mutex;
    std::string web_search_url_override;
    std::unique_ptr<AssistantService> assistant;
    std::shared_ptr<const std::string> maintenance_prompt;
    Providers providers;
    // Live sessions can update audio while they finish shutting down.
    std::unique_ptr<AudioDownloadManager> audio_downloads;
    std::unique_ptr<LiveSessionManager> live_sessions;
    VoiceOutputProxy speech_proxy;
    MediaResources media_resources;
    std::optional<std::string> speech_url_override;
    PendingMediaRegistry pending_media{media_resources};
    BackgroundJobs background_jobs;
    XaiVoiceSessions xai_voice{background_jobs};
    mutable std::timed_mutex lifecycle_mutex;
    std::atomic_bool stopping_flag{};
    // Set from the maintenance copy of an R2 upload until its transfer ends.
    std::atomic_bool vault_upload_running{};
    std::atomic_bool vault_command_running{};
    bool running{};
    bool stopped{};
    std::atomic_bool unusable{};
    std::atomic_bool preserve_on_destroy{};
    std::atomic<ApplicationState> state{ApplicationState::running};
    std::atomic_uint64_t published_epoch{1};
    std::atomic_uint published_capabilities{};
    ResourceHooks resource_hooks;
    ContextChanged context_changed;
    std::uint64_t notified_epoch{};
    ApplicationState notified_state{ApplicationState::running};
    VaultMaintenance vault_maintenance{*this};
    void start_sync_worker(SyncStart mode);
    void resume_sync_worker();
    // Declared after its borrowed dependencies; BackgroundJobs joins in ~Impl.
    std::unique_ptr<NotesService> notes;
    D1Client::Transport d1_test_transport;
    std::mutex notes_changed_mutex;
    NotesChanged notes_changed;
    std::unique_ptr<D1SyncWorker> sync_worker;
};

} // namespace cha::app
