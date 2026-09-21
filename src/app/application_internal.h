#pragma once

// Private runtime shared by the application, vault, and media implementations.
#include "app/application.h"
#include "app/background_jobs.h"
#include "media/pending_media_registry.h"
#include "providers/api_key_store.h"
#include "providers/openai_oauth.h"
#include "providers/providers.h"
#include "storage/session_repository.h"
#include "app/current_vault.h"
#include "session/session_mirror.h"

#include <atomic>

namespace cha::app {

struct Application::Impl {
    explicit Impl(
        const ApplicationCommand& selected_command,
        std::string selected_vault_password,
        RuntimeSettings selected_settings);

    void mark_unusable();

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
        R2DatabaseTransfer upload_database();
        R2DatabaseTransfer download_database();
        WorkspaceConfigTransfer import_configuration();
        WorkspaceConfigTransfer export_configuration();

        void publish_vault_names();
    private:
        void publish_vault(VaultDefinition vault);
        std::chrono::milliseconds maintenance_grace() const;
        std::uint64_t publish_epoch(
            PendingContextNotice& notice,
            ApplicationState next_state);
        // Caller holds lifecycle_mutex. False keeps the previous vault selected.
        bool drain_for_maintenance(
            PendingContextNotice& notice,
            bool cancel_audio = true);
        void cancel_maintenance_locked(PendingContextNotice& notice);
        void end_maintenance_locked(bool available, PendingContextNotice& notice);
        void reopen(
            WorkspaceConfigStore::MaintenanceGuard& database,
            const SessionRepository::MaintenanceGuard& repository);
        void reopen_after_failure(
            WorkspaceConfigStore::MaintenanceGuard& database,
            const SessionRepository::MaintenanceGuard& repository);
        void rebuild_mirror(const std::optional<std::filesystem::path>& root);
        void protect_active_database(
            std::string password,
            PendingContextNotice& notice);
        template<typename Operation>
        auto maintain_database(Operation operation, bool cancel_audio = true);

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
    Providers providers;
    std::unique_ptr<LiveSessionManager> live_sessions;
    std::unique_ptr<AudioDownloadManager> audio_downloads;
    FishAudioProxy speech_proxy;
    MediaResources media_resources;
    std::optional<std::string> speech_url_override;
    PendingMediaRegistry pending_media{media_resources};
    BackgroundJobs background_jobs;
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
