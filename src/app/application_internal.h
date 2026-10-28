#pragma once

// Private runtime shared by the application, vault, and media implementations.
#include "app/application.h"
#include "app/assistant_service.h"
#include "app/background_jobs.h"
#include "app/xai_voice_session.h"
#include "media/pending_media_registry.h"
#include "providers/api_key_store.h"
#include "providers/folder_store_client.h"
#include "providers/openai_oauth.h"
#include "providers/providers.h"
#include "storage/session_repository.h"
#include "app/current_vault.h"
#include "session/session_mirror.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <set>

namespace cha::app {

struct Application::Impl {
    explicit Impl(
        const ApplicationCommand& selected_command,
        std::string selected_vault_password,
        RuntimeSettings selected_settings);

    static constexpr unsigned can_modify = 1U << 0;
    static constexpr unsigned can_transfer_r2 = 1U << 1;

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
    struct VaultMaintenance {
        explicit VaultMaintenance(Impl& application) : app(application) {}

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
        MaintenanceResult merge_parent_vault(
            std::string password,
            std::uint64_t epoch,
            const std::function<bool()>& cancelled);
        R2UploadCheck check_database_upload(std::uint64_t epoch) const;
        R2DatabaseTransfer upload_database(
            std::optional<std::string> expected_etag,
            std::uint64_t epoch);
        R2DatabaseTransfer download_database(std::uint64_t epoch);
        WorkspaceConfigTransfer import_configuration(std::uint64_t epoch);
        WorkspaceConfigTransfer export_configuration(std::uint64_t epoch);

        void publish_vault_names();
        void delete_forum(std::string_view forum_id, std::uint64_t epoch);
    private:
        MaintenanceResult merge(
            std::string_view source_name,
            std::string password,
            std::uint64_t epoch,
            bool download_parent,
            const std::function<bool()>& cancelled);
        void publish_vault(VaultDefinition vault);
        std::chrono::milliseconds maintenance_grace() const;
        std::uint64_t publish_epoch(
            PendingContextNotice& notice,
            ApplicationState next_state);
        // Caller holds lifecycle_mutex. False keeps the previous vault selected.
        bool drain_for_maintenance(
            PendingContextNotice& notice,
            bool cancel_audio = true, bool require_main = true);
        void cancel_maintenance_locked(PendingContextNotice& notice);
        void end_maintenance_locked(bool available, PendingContextNotice& notice);
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
            bool cancel_audio = true);

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
    // Network work waits outside lifecycle_mutex and observes its cancellation flag.
    std::timed_mutex audio_submission_mutex;
    std::timed_mutex compression_mutex;
    struct CompressionControl {
        std::shared_ptr<std::atomic_bool> cancel = std::make_shared<std::atomic_bool>(false);
        std::string name;
    };
    // All compression state and retirement use lifecycle_mutex.
    std::map<std::int64_t, std::shared_ptr<CompressionControl>> compressions;
    NoteAudioChanged note_compression_changed;
    void notify_compression(std::int64_t id, std::uint64_t epoch) noexcept;
    void retire_compression(std::int64_t id, const std::shared_ptr<CompressionControl>& control,
        std::uint64_t epoch, EventSeverity severity, const std::string& message) noexcept;
    void cancel_compressions();
    nlohmann::json note_compression_status(std::int64_t id); // Caller holds lifecycle_mutex.
    std::mutex background_audio_mutex;
    std::map<std::int64_t, std::shared_ptr<std::atomic_bool>> background_audio;
    // Guarded by lifecycle_mutex; retained across worker replacement.
    std::set<std::int64_t> background_audio_warned;
    NoteAudioChanged note_audio_changed;
    void interrupt_note_audio();
    void remove_note_audio_control(std::int64_t id, const std::shared_ptr<std::atomic_bool>& cancel);
    void resume_note_audio(); // Schedule after maintenance releases its store guard.
    void recover_note_audio(); // Caller holds lifecycle_mutex.
    nlohmann::json background_note_audio_status(std::int64_t id); // Caller holds lifecycle_mutex.
    std::shared_ptr<std::atomic_bool> launch_note_audio(NoteAudioAttempt attempt, std::string voice = {}, std::string jsonl = {});
    void run_note_audio(NoteAudioAttempt attempt, std::uint64_t epoch,
        std::shared_ptr<std::atomic_bool> cancel, std::string voice, std::string jsonl);
    void interrupt_abandoned_audio();
    enum class FolderStoreOpKind { upload, remove, folder_delete };
    enum class FolderStorePhase {
        preparing,
        uploading,
        indexing,
        cleaning,
        removing,
        completed,
        failed,
        cancelled,
        warning
    };
    struct FolderStoreControl {
        std::int64_t operation_id{};
        std::uint64_t epoch{};
        FolderStoreOpKind kind{FolderStoreOpKind::upload};
        std::int64_t root_id{};
        std::optional<std::int64_t> store_record_id;
        FolderStorePhase phase{FolderStorePhase::preparing};
        std::int64_t completed{};
        std::int64_t total{};
        std::string error;
        std::string result;
        bool terminal{};
        // Retained across failed-delete retries, even after a warned store is gone.
        bool warned{};
        bool known_cleanup_remains{};
        bool delete_failed{};
        bool folder_deleted{};
        std::shared_ptr<std::atomic_bool> cancel_forward =
            std::make_shared<std::atomic_bool>(false);
        std::shared_ptr<std::atomic_bool> cancel_all =
            std::make_shared<std::atomic_bool>(false);
        std::mutex wait_mutex;
        std::condition_variable wait_cv;
        std::string vault_name;
        std::string folder_name;
        FolderStoreService service{FolderStoreService::openai};
        std::string api_key_id;
        std::optional<std::string> unsaved_store_id;
        std::vector<std::string> unsaved_file_ids;
    };
    std::map<std::int64_t, std::shared_ptr<FolderStoreControl>> folder_store_controls;
    std::int64_t next_folder_store_operation{1};
    FolderStoreTransport folder_store_transport;
    void interrupt_folder_stores();
    [[nodiscard]] bool folder_in_delete_subtree(std::int64_t folder_id);
    [[nodiscard]] bool folder_store_reserved(std::int64_t folder_id);
    [[nodiscard]] bool folder_store_blocks_delete(std::int64_t item_id);
    [[nodiscard]] std::shared_ptr<FolderStoreControl> folder_store_operation(std::int64_t operation_id);
    void retire_deleted_note_work(const std::vector<std::int64_t>& note_ids, std::uint64_t epoch);
    void release_folder_store_control_locked(const std::shared_ptr<FolderStoreControl>& control);
    [[nodiscard]] bool folder_store_lifetime_locked(
        const std::shared_ptr<FolderStoreControl>& control,
        std::optional<std::int64_t> store_id = {});
    [[nodiscard]] bool folder_store_lifetime(
        const std::shared_ptr<FolderStoreControl>& control,
        std::optional<std::int64_t> store_id = {});
    void run_folder_store_upload(std::shared_ptr<FolderStoreControl> control);
    void run_folder_store_remove(std::shared_ptr<FolderStoreControl> control);
    void run_folder_store_delete(std::shared_ptr<FolderStoreControl> control, bool retry);
    bool launch_folder_store_control(
        const std::shared_ptr<FolderStoreControl>& control, std::function<void()> work);
    XaiVoiceSessions xai_voice{background_jobs};
    mutable std::timed_mutex lifecycle_mutex;
    std::atomic_bool stopping_flag{};
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
};

} // namespace cha::app
