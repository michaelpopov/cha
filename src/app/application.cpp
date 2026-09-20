#include "app/application.h"

#include "app/media_operations.h"
#include "app/settings_operations.h"
#include "app/vault_operations.h"
#include "app/workspace_operations.h"
#include "providers/openai_oauth.h"
#include "providers/api_key_store.h"
#include "providers/credentials.h"
#include "providers/provider_client.h"
#include "providers/providers.h"
#include "session/not_found_error.h"
#include "session/session_label.h"
#include "session/session_lease.h"
#include "session/session_repository.h"
#include "session/workspace_session_database.h"
#include "util/environment.h"
#include "util/logging.h"
#include "util/path_name.h"
#include "util/public_name.h"
#include "util/text.h"
#include "util/toml_file.h"
#include "web/current_vault.h"
#include "web/session_markdown.h"
#include "web/session_mirror.h"
#include "web/session_projection.h"
#include "workspace/builtins.h"
#include "workspace/session_open.h"
#include "workspace/workspace.h"
#include "workspace/workspace_config_store.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <atomic>
#include <exception>
#include <functional>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

using cha::web::WebSettings;
using cha::web::ErrorCode;
using cha::web::VaultCreate;
using cha::web::VaultDefinition;
using cha::web::VaultRegistrySnapshot;
using cha::web::VaultUpdate;
using cha::web::UnknownVaultError;
using cha::web::VaultPasswordError;
using cha::web::find_vault;
using cha::web::load_vault_definition_file;
using cha::web::require_openable_protected_database;
using cha::web::require_switchable_database;
using cha::web::same_vault_name;
using cha::WorkspaceRestartRequiredError;
using cha::rewrite_toml_file;
using cha::inspect_workspace_session_database;
using cha::WorkspaceDatabaseState;

namespace cha::app {

ApplicationError::ApplicationError(ErrorCode code, std::string message)
    : std::runtime_error(
          message.empty() ? "The application operation failed" : std::move(message)),
      code(code) {}

bool OperationReply::complete(nlohmann::json result) {
    std::function<void()> callback;
    {
        std::lock_guard lock(mutex_);
        if (result_ || abandoned_) return false;
        result_ = std::move(result);
        callback = std::move(ready_callback_);
    }
    ready_.notify_all();
    if (callback) callback();
    return true;
}

bool OperationReply::fail(ErrorCode code, std::string message) {
    std::function<void()> callback;
    {
        std::lock_guard lock(mutex_);
        if (result_ || abandoned_) return false;
        result_ = Failure{code, std::move(message)};
        callback = std::move(ready_callback_);
    }
    ready_.notify_all();
    if (callback) callback();
    return true;
}

void OperationReply::set_ready_callback(std::function<void()> callback) {
    {
        std::lock_guard lock(mutex_);
        if (abandoned_) return;
        if (!result_) {
            ready_callback_ = std::move(callback);
            return;
        }
    }
    if (callback) callback();
}

std::optional<OperationReply::Result> OperationReply::peek() const {
    std::lock_guard lock(mutex_);
    return result_;
}

void OperationReply::abandon() const {
    std::lock_guard lock(mutex_);
    abandoned_ = true;
    ready_callback_ = {};
}

std::string_view application_state_name(ApplicationState state) noexcept {
    switch (state) {
    case ApplicationState::running:
        return "running";
    case ApplicationState::maintenance:
        return "maintenance";
    case ApplicationState::stopping:
        return "stopping";
    case ApplicationState::unavailable:
        return "unavailable";
    }
    return "unavailable";
}

namespace {

ProviderClientFactory shared_openai_provider_factory(
    OpenAiOAuth* oauth,
    ApiKeyStore* api_keys) {
    return [oauth, api_keys](SharedCharacterDefinition definition) {
        return std::make_unique<ProviderClient>(
            std::move(definition), oauth, api_keys);
    };
}

std::shared_ptr<const Workspace> current_workspace() {
    std::shared_ptr<const Workspace> workspace = getws();
    if (!workspace) throw std::runtime_error("Workspace is not loaded");
    return workspace;
}

cha::web::CharacterSummary character_summary(
    const Workspace& workspace,
    const WorkspaceCharacter& character) {
    return {
        .id = character.character.id,
        .display_name = character.character.display_name,
        .description = character.character.description,
        .appearance = character.character.appearance,
        .voice = cha::web::resolve_speech_voice(workspace, character),
    };
}

cha::web::PersonaSummary persona_summary(
    const Workspace& workspace,
    const WorkspacePersona& persona) {
    return {
        .id = persona.id,
        .display_name = persona.display_name,
        .description = persona.description,
        .appearance = persona.appearance,
        .voice = cha::web::resolve_speech_voice(workspace, persona),
    };
}

cha::web::ForumSummary forum_summary(
    const WorkspaceForum& forum,
    const Workspace& workspace) {
    const WorkspacePersona* persona =
        workspace.find_persona(forum.default_persona_id);
    if (persona == nullptr) {
        throw std::runtime_error(
            "Forum default persona is absent from the workspace");
    }
    cha::web::ForumSummary result{
        .id = forum.id,
        .display_name = forum.display_name,
        .description = forum.description,
        .default_character_id = forum.default_character_id,
        .default_persona_id = forum.default_persona_id,
        .default_persona_display_name = persona->display_name,
    };
    result.members.reserve(forum.members.size());
    for (const WorkspaceForumMember& member : forum.members) {
        const WorkspaceCharacter* character =
            workspace.find_character(member.character_id);
        if (character == nullptr) {
            throw std::runtime_error(
                "Forum member is absent from the workspace");
        }
        result.members.push_back(character_summary(workspace, *character));
    }
    std::ranges::sort(
        result.members, {},
        [](const cha::web::CharacterSummary& character) {
            return fold_ascii(character.display_name);
        });
    return result;
}

cha::web::Bootstrap make_bootstrap(
    const Workspace& workspace,
    const std::vector<StoredSession>& recent,
    const FullSessionId& initial,
    std::string vault_name,
    std::vector<std::string> vaults) {
    cha::web::Bootstrap bootstrap{
        .initial_forum_id = initial.forum_id,
        .initial_session_id = initial.session_id};
    for (const WorkspacePersona& persona : workspace.personas()) {
        bootstrap.personas.push_back(persona_summary(workspace, persona));
    }
    for (const WorkspaceCharacter& character : workspace.characters()) {
        bootstrap.characters.push_back(character_summary(workspace, character));
    }
    for (const WorkspaceForum& forum : workspace.forums()) {
        bootstrap.forums.push_back(forum_summary(forum, workspace));
    }
    bootstrap.recent_sessions.reserve(recent.size());
    for (const StoredSession& stored : recent) {
        bootstrap.recent_sessions.push_back({
            stored.identity.forum_id,
            stored.identity.session_id,
            stored.label,
            stored.updated_at});
    }
    bootstrap.vault_name = std::move(vault_name);
    bootstrap.vaults = std::move(vaults);
    return bootstrap;
}

FullSessionId fallback_session(const SessionRepository& sessions) {
    const FullSessionId welcome{
        std::string(entrance_id), std::string(welcome_id)};
    try {
        sessions.validate(welcome);
        return welcome;
    } catch (const std::exception&) {
    }
    const auto recent = sessions.recent();
    if (!recent.empty()) return recent.front().identity;
    return welcome;
}

} // namespace

cha::web::WebSettings native_settings() {
    return {};
}

struct Application::Impl {
    explicit Impl(
        const cha::web::ApplicationCommand& selected_command,
        std::string selected_vault_password,
        WebSettings selected_settings)
        : command(selected_command),
          active_password(std::move(selected_vault_password)),
          settings(std::move(selected_settings)),
          current_vault_(selected_command.vault),
          store(WorkspaceConfigStore::open(
              command.vault.data, active_password)),
          api_keys(std::make_unique<ApiKeyStore>(
              *store,
              command.config_directory / "api-keys.json")),
          openai_auth(std::make_unique<OpenAiOAuth>(
              command.config_directory / "openai-auth.json")),
          providers(shared_openai_provider_factory(
              openai_auth.get(), api_keys.get())) {
        publish_vault_names();
        const auto seed = TemporarySessionSeed{
            {std::string(entrance_id), std::string(welcome_id)},
            std::string(welcome_name)};
        sessions = std::make_shared<SessionRepository>(
            store->database_path(),
            store->workspace_path(),
            store->welcome_path(),
            seed,
            active_password);
        mirror = std::make_shared<cha::web::SessionMirror>();
        if (const auto root = cha::web::session_mirror_root(command.vault)) {
            try {
                mirror->rebuild(root, *sessions);
            } catch (const std::exception& error) {
                log_warn(
                    "Session mirror rebuild failed: "
                    + std::string(error.what()));
                mirror->rebuild(std::nullopt, *sessions);
            }
        }

        auto opener = [this](
                          const FullSessionId& identity,
                          std::shared_ptr<WakeNotifier> notifier) {
            OpenedSession opened = cha::open_session(
                *sessions, identity, providers, std::move(notifier), *store);
            const auto selected_mirror = mirror;
            opened.mirror = [selected_mirror, identity](
                                std::string_view label,
                                std::span<const TranscriptEntry> entries) {
                selected_mirror->update(identity, label, entries);
            };
            return opened;
        };
        live_sessions = std::make_unique<cha::web::LiveSessionManager>(
            settings, opener);
        audio_downloads = std::make_unique<cha::web::AudioDownloadManager>(
            *sessions, current_vault_, true);
        publish_capabilities_locked();
        running = true;
        notified_epoch = live_sessions->context_epoch();
        published_epoch.store(notified_epoch);
    }

    void publish_vault_names() {
        std::vector<std::string> names;
        names.reserve(command.vaults.size());
        for (const cha::web::VaultDefinition& vault : command.vaults) {
            names.push_back(vault.name);
        }
        current_vault_.set_names(std::move(names));
    }

    void publish_vault(VaultDefinition vault) {
        std::vector<std::string> names;
        names.reserve(command.vaults.size());
        for (const VaultDefinition& configured : command.vaults) {
            names.push_back(configured.name);
        }
        current_vault_.set(std::move(vault), std::move(names));
    }

    static constexpr unsigned can_modify = 1U << 0;
    static constexpr unsigned can_transfer_r2 = 1U << 1;

    void publish_capabilities_locked() {
        unsigned capabilities = 0;
        if (current_vault_.get().modify) capabilities |= can_modify;
        if (api_keys->r2()) capabilities |= can_transfer_r2;
        published_capabilities.store(capabilities);
    }

    [[nodiscard]] ApplicationCapabilities capabilities() const noexcept {
        const auto epoch = published_epoch.load();
        if (state.load() != ApplicationState::running) return {};
        const unsigned capabilities = published_capabilities.load();
        if (state.load() != ApplicationState::running
            || published_epoch.load() != epoch) {
            return {};
        }
        return {
            .can_modify = (capabilities & can_modify) != 0,
            .can_transfer_r2 = (capabilities & can_transfer_r2) != 0,
        };
    }

    [[nodiscard]] std::optional<ErrorCode> admit_locked(std::uint64_t epoch) const {
        if (unusable || stopping_flag || stopped) {
            return ErrorCode::application_unavailable;
        }
        const ApplicationState current_state = state.load();
        if (current_state == ApplicationState::maintenance) {
            return ErrorCode::vault_changed;
        }
        if (current_state != ApplicationState::running) {
            return ErrorCode::application_unavailable;
        }
        if (epoch != 0 && epoch != published_epoch.load()) {
            return ErrorCode::vault_changed;
        }
        return std::nullopt;
    }

    void require_admitted(std::uint64_t epoch) const {
        if (const auto error = admit_locked(epoch)) {
            throw ApplicationError(*error);
        }
    }

    std::chrono::milliseconds maintenance_grace() const {
        return command.test_shutdown_grace_ms
            ? std::chrono::milliseconds(*command.test_shutdown_grace_ms)
            : settings.shutdown_grace;
    }

    void pause_resources(bool cancel) {
        cancel_all_pending_media();
        media_resources.revoke_all();
        if (audio_downloads) audio_downloads->pause(cancel);
        if (resource_hooks.pause) resource_hooks.pause(cancel);
    }

    void resume_resources() {
        if (unusable) return;
        if (audio_downloads) audio_downloads->resume();
        if (resource_hooks.resume) resource_hooks.resume();
    }

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

    void take_context_notice(PendingContextNotice& notice) {
        const auto epoch = published_epoch.load();
        const ApplicationState current_state = state.load();
        if (notified_epoch == epoch && notified_state == current_state) return;
        notified_epoch = epoch;
        notified_state = current_state;
        notice = {context_changed, epoch, current_state};
    }

    std::uint64_t publish_epoch(
        PendingContextNotice& notice,
        ApplicationState next_state) {
        const auto epoch = live_sessions->bump_context_epoch();
        published_epoch.store(epoch);
        ApplicationState expected = ApplicationState::maintenance;
        (void)state.compare_exchange_strong(expected, next_state);
        take_context_notice(notice);
        return epoch;
    }

    // Caller holds lifecycle_mutex. False leaves the previous vault selected.
    bool drain_for_maintenance(
        PendingContextNotice& notice,
        bool cancel_audio = true) {
        if (unusable) {
            throw WorkspaceRestartRequiredError(
                "The workspace database could not be reopened after an earlier "
                "maintenance operation. Restart is required");
        }
        if (stopping_flag || stopped
            || state.load() != ApplicationState::running) {
            throw std::runtime_error("CHA application is unavailable");
        }
        state.store(ApplicationState::maintenance);
        cha::web::GlobalMaintenanceResult reserved = [&] {
            try {
                return live_sessions->reserve_global_maintenance(
                    maintenance_grace());
            } catch (...) {
                state.store(ApplicationState::running);
                throw;
            }
        }();
        if (std::holds_alternative<cha::web::MaintenanceFailure>(reserved)) {
            // Reservation may already have stopped one or more actors. Keep
            // old queued work from treating the recovered context as intact.
            publish_epoch(notice, ApplicationState::running);
            return false;
        }
        global_maintenance = std::move(
            std::get<cha::web::LiveSessionGlobalMaintenance>(reserved));
        try {
            pause_resources(cancel_audio);
        } catch (...) {
            cancel_maintenance_locked(notice);
            throw;
        }
        return true;
    }

    void cancel_maintenance_locked(PendingContextNotice& notice) {
        global_maintenance.reset();
        publish_capabilities_locked();
        publish_epoch(notice, ApplicationState::running);
        if (state.load() != ApplicationState::running
            || stopping_flag.load()) {
            return;
        }
        resume_resources();
        if (stopping_flag.load()) {
            state.store(ApplicationState::stopping);
            pause_resources(true);
            take_context_notice(notice);
        }
    }

    void end_maintenance_locked(bool available, PendingContextNotice& notice) {
        global_maintenance.reset();
        if (!available) {
            unusable = true;
            publish_epoch(notice, ApplicationState::unavailable);
            return;
        }
        publish_capabilities_locked();
        publish_epoch(notice, ApplicationState::running);
        if (state.load() != ApplicationState::running
            || stopping_flag.load()) {
            return;
        }
        resume_resources();
        if (stopping_flag.load()) {
            state.store(ApplicationState::stopping);
            pause_resources(true);
            take_context_notice(notice);
        }
    }

    void reopen(
        WorkspaceConfigStore::MaintenanceGuard& database,
        const SessionRepository::MaintenanceGuard& repository) {
        try {
            database.reopen();
            repository.synchronize_forums(*current_workspace());
        } catch (...) {
            unusable = true;
            throw;
        }
    }

    void reopen_after_failure(
        WorkspaceConfigStore::MaintenanceGuard& database,
        const SessionRepository::MaintenanceGuard& repository) {
        try {
            reopen(database, repository);
        } catch (const std::exception& error) {
            log_critical(
                "Failed to reopen workspace database after maintenance: "
                + std::string(error.what()));
            throw WorkspaceRestartRequiredError(error.what());
        }
    }

    void rebuild_mirror(const std::optional<std::filesystem::path>& root) {
        try {
            mirror->rebuild(root, *sessions);
        } catch (const std::exception& error) {
            log_warn(
                "Session mirror rebuild failed: " + std::string(error.what()));
            mirror->rebuild(std::nullopt, *sessions);
        }
    }

    void protect_active_database(
        std::string password,
        PendingContextNotice& notice) {
        if (!drain_for_maintenance(notice)) {
            throw std::runtime_error(
                "Could not pause active sessions for database maintenance");
        }
        bool database_closed = false;
        try {
            auto database = store->reserve_maintenance();
            SessionRepository::MaintenanceGuard repository =
                sessions->reserve_maintenance();
            const std::filesystem::path database_path = current_vault_.get().data;
            repository.checkpoint();
            database.close();
            database_closed = true;
            try {
                protect_workspace_session_database(database_path, password);
            } catch (...) {
                database.set_password(active_password);
                try {
                    reopen_after_failure(database, repository);
                    end_maintenance_locked(true, notice);
                } catch (...) {
                    end_maintenance_locked(false, notice);
                    throw;
                }
                throw;
            }
            mirror->rebuild(std::nullopt, *sessions);
            active_password = std::move(password);
            database.set_password(active_password);
            repository.retarget(database_path, active_password);
            try {
                reopen(database, repository);
                end_maintenance_locked(true, notice);
            } catch (...) {
                end_maintenance_locked(false, notice);
                throw;
            }
        } catch (...) {
            if (state.load() == ApplicationState::maintenance) {
                if (database_closed) {
                    end_maintenance_locked(false, notice);
                } else {
                    cancel_maintenance_locked(notice);
                }
            }
            throw;
        }
    }

    template<typename Operation>
    auto maintain_database(Operation operation, bool cancel_audio = true) {
        PendingContextNotice notice;
        using Result = decltype(operation());
        std::optional<Result> result;
        std::exception_ptr error;
        try {
            const std::lock_guard lifecycle(lifecycle_mutex);
            if (!drain_for_maintenance(notice, cancel_audio)) {
                throw std::runtime_error(
                    "Could not pause active sessions for database maintenance");
            }
            try {
                auto database = store->reserve_maintenance();
                SessionRepository::MaintenanceGuard repository =
                    sessions->reserve_maintenance();
                repository.checkpoint();
                database.close();
                bool reopened = false;
                try {
                    result = operation();
                    reopen(database, repository);
                    reopened = true;
                    end_maintenance_locked(true, notice);
                } catch (...) {
                    if (!reopened) {
                        try {
                            reopen_after_failure(database, repository);
                            end_maintenance_locked(true, notice);
                        } catch (...) {
                            end_maintenance_locked(false, notice);
                            error = std::current_exception();
                        }
                        if (!error) error = std::current_exception();
                    } else {
                        end_maintenance_locked(false, notice);
                        error = std::current_exception();
                    }
                }
            } catch (...) {
                if (state.load() == ApplicationState::maintenance) {
                    cancel_maintenance_locked(notice);
                }
                throw;
            }
        } catch (...) {
            notice.dispatch();
            throw;
        }
        notice.dispatch();
        if (error) std::rethrow_exception(error);
        return std::move(*result);
    }

    bool launch_background(std::function<void(std::atomic_bool&)> work) {
        auto control = std::make_shared<BackgroundControl>();
        std::lock_guard lock(background_mutex);
        if (background_closed) return false;
        reap_finished_locked();
        background_jobs.push_back({std::thread{}, control});
        try {
            background_jobs.back().worker = std::thread(
                [work = std::move(work), control] {
                    work(control->cancel);
                    {
                        std::lock_guard lock(control->mutex);
                        control->finished.store(true);
                    }
                    control->changed.notify_all();
                });
        } catch (...) {
            background_jobs.pop_back();
            throw;
        }
        return true;
    }

    void join_background() {
        for (;;) {
            std::vector<BackgroundJob> jobs;
            {
                std::lock_guard lock(background_mutex);
                background_closed = true;
                jobs.swap(background_jobs);
            }
            if (jobs.empty()) return;
            for (auto& job : jobs) {
                job.control->cancel.store(true);
                if (job.worker.joinable()) job.worker.join();
            }
        }
    }

    bool join_background_until(
        std::chrono::steady_clock::time_point deadline) {
        std::vector<std::shared_ptr<BackgroundControl>> controls;
        {
            std::lock_guard lock(background_mutex);
            background_closed = true;
            controls.reserve(background_jobs.size());
            for (const BackgroundJob& job : background_jobs) {
                job.control->cancel.store(true);
                controls.push_back(job.control);
            }
        }
        for (const auto& control : controls) {
            std::unique_lock lock(control->mutex);
            if (!control->changed.wait_until(lock, deadline, [&] {
                    return control->finished.load();
                })) {
                return false;
            }
        }

        std::vector<BackgroundJob> jobs;
        {
            std::lock_guard lock(background_mutex);
            jobs.swap(background_jobs);
        }
        // finished means all access to Application-owned state is complete.
        // Detaching here keeps the deadline strict while the thread tears down
        // its own harmless captures.
        for (BackgroundJob& job : jobs) {
            if (job.worker.joinable()) job.worker.detach();
        }
        return true;
    }

    void reap_finished_locked() {
        const auto first_live = std::remove_if(
            background_jobs.begin(),
            background_jobs.end(),
            [](BackgroundJob& job) {
                if (!job.control->finished.load()) return false;
                if (job.worker.joinable()) job.worker.detach();
                return true;
            });
        background_jobs.erase(first_live, background_jobs.end());
    }

    ~Impl() {
        speech_proxy.stop();
        cancel_all_pending_media();
        if (audio_downloads) audio_downloads->request_stop();
        join_background();
        if (running && !stopped) {
            live_sessions->begin_shutdown();
            (void)live_sessions->join_shutdown(settings.shutdown_grace);
        }
        providers.shutdown();
    }

    cha::web::ApplicationCommand command;
    std::string active_password;
    WebSettings settings;
    cha::web::CurrentVault current_vault_;
    std::unique_ptr<WorkspaceConfigStore> store;
    std::shared_ptr<SessionRepository> sessions;
    std::shared_ptr<cha::web::SessionMirror> mirror;
    std::unique_ptr<ApiKeyStore> api_keys;
    std::unique_ptr<OpenAiOAuth> openai_auth;
    Providers providers;
    std::unique_ptr<cha::web::LiveSessionManager> live_sessions;
    std::unique_ptr<cha::web::AudioDownloadManager> audio_downloads;
    cha::web::FishAudioProxy speech_proxy;
    MediaResources media_resources;
    std::optional<std::string> speech_url_override;
    struct PendingMedia {
        std::string connection_id;
        std::uint64_t request_id{};
        std::shared_ptr<std::atomic_bool> cancelled;
        std::string resource_id;
    };
    std::mutex media_mutex;
    std::map<std::pair<std::string, std::uint64_t>, std::shared_ptr<PendingMedia>>
        pending_media;

    std::shared_ptr<PendingMedia> remember_pending(
        std::string connection_id,
        std::uint64_t request_id) {
        auto pending = std::make_shared<PendingMedia>();
        pending->connection_id = std::move(connection_id);
        pending->request_id = request_id;
        pending->cancelled = std::make_shared<std::atomic_bool>(false);
        std::lock_guard lock(media_mutex);
        pending_media[{pending->connection_id, request_id}] = pending;
        return pending;
    }

    void forget_pending(
        const std::shared_ptr<PendingMedia>& pending,
        bool keep_resource = false) {
        std::lock_guard lock(media_mutex);
        const auto found = pending_media.find(
            {pending->connection_id, pending->request_id});
        if (found != pending_media.end() && found->second == pending
            && (!keep_resource || pending->resource_id.empty())) {
            pending_media.erase(found);
        }
    }

    struct PendingMediaCleanup {
        Impl* owner;
        std::shared_ptr<PendingMedia> pending;

        ~PendingMediaCleanup() {
            // A completed speech reply can still be queued in the bridge.
            // Keep its cancellation association until release or cancellation.
            owner->forget_pending(pending, true);
        }
    };

    bool set_pending_resource(
        const std::shared_ptr<PendingMedia>& pending,
        std::string resource_id) {
        std::lock_guard lock(media_mutex);
        const auto found = pending_media.find(
            {pending->connection_id, pending->request_id});
        if (found == pending_media.end() || found->second != pending
            || pending->cancelled->load()) {
            return false;
        }
        pending->resource_id = std::move(resource_id);
        return true;
    }

    void cancel_pending(
        std::string_view connection_id,
        std::uint64_t request_id) {
        std::shared_ptr<PendingMedia> pending;
        std::string resource_id;
        {
            std::lock_guard lock(media_mutex);
            const auto found = pending_media.find(
                {std::string(connection_id), request_id});
            if (found == pending_media.end()) return;
            pending = found->second;
            pending->cancelled->store(true);
            resource_id = std::move(pending->resource_id);
            pending_media.erase(found);
        }
        if (!resource_id.empty()) {
            media_resources.release(connection_id, resource_id);
        }
    }

    void cancel_connection_media(std::string_view connection_id) {
        {
            std::lock_guard lock(media_mutex);
            for (auto item = pending_media.begin();
                 item != pending_media.end();) {
                if (item->second->connection_id != connection_id) {
                    ++item;
                    continue;
                }
                item->second->cancelled->store(true);
                item = pending_media.erase(item);
            }
        }
        media_resources.revoke_connection(connection_id);
    }

    void cancel_all_pending_media() {
        std::lock_guard lock(media_mutex);
        for (auto& [key, item] : pending_media) {
            item->cancelled->store(true);
        }
        pending_media.clear();
    }

    struct BackgroundControl {
        std::atomic_bool cancel{};
        std::atomic_bool finished{};
        std::mutex mutex;
        std::condition_variable changed;
    };
    struct BackgroundJob {
        std::thread worker;
        std::shared_ptr<BackgroundControl> control;
    };
    std::mutex background_mutex;
    std::vector<BackgroundJob> background_jobs;
    bool background_closed{};
    mutable std::timed_mutex lifecycle_mutex;
    std::atomic_bool stopping_flag{};
    bool running{};
    bool stopped{};
    bool unusable{};
    std::atomic_bool preserve_on_destroy{};
    std::atomic<ApplicationState> state{ApplicationState::running};
    std::atomic_uint64_t published_epoch{1};
    std::atomic_uint published_capabilities{};
    ResourceHooks resource_hooks;
    ContextChanged context_changed;
    std::uint64_t notified_epoch{};
    ApplicationState notified_state{ApplicationState::running};
    std::optional<cha::web::LiveSessionGlobalMaintenance> global_maintenance;
};

Application::Application(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Application::~Application() {
    if (impl_ && impl_->preserve_on_destroy.load()) {
        // A worker that ignored cancellation still owns pointers into Impl.
        // The process is already stopping, so retaining Impl is safer than
        // either blocking destruction or detaching it from its dependencies.
        (void)impl_.release();
    }
}

std::unique_ptr<Application> Application::open(
    const cha::web::ApplicationCommand& command,
    std::string vault_password,
    WebSettings settings) {
    for (const std::string& warning : command.warnings) log_warn(warning);
    load_dotenv(command.config_directory / ".env");
    if (command.vault.password_protected && vault_password.empty()) {
        throw cha::web::VaultPasswordError(
            "Password required to open this vault");
    }
    if (command.vault.password_protected) {
        cha::web::require_openable_protected_database(
            command.vault.data, vault_password);
    }
    return std::unique_ptr<Application>(new Application(
        std::make_unique<Impl>(
            command, std::move(vault_password), std::move(settings))));
}

ApplicationBootstrap Application::bootstrap() {
    ApplicationBootstrap result;
    const auto read_published_state = [&] {
        const ApplicationState state = impl_->state.load();
        return state == ApplicationState::stopping
            ? ApplicationState::unavailable
            : state;
    };
    result.state = read_published_state();
    result.capabilities = impl_->capabilities();
    if (result.state != ApplicationState::running) {
        result.context_epoch = impl_->published_epoch.load();
        auto [vault, names] = impl_->current_vault_.snapshot();
        result.presentation.vault_name = std::move(vault.name);
        result.presentation.vaults = std::move(names);
        return result;
    }

    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    result.context_epoch = impl_->published_epoch.load();
    result.state = read_published_state();
    result.capabilities = impl_->capabilities();
    auto [vault, names] = impl_->current_vault_.snapshot();
    if (names.empty()) {
        for (const auto& definition : impl_->command.vaults) {
            names.push_back(definition.name);
        }
    }
    result.presentation.vault_name = vault.name;
    result.presentation.vaults = names;
    if (result.state != ApplicationState::running) return result;
    const auto workspace = current_workspace();
    FullSessionId initial = fallback_session(*impl_->sessions);
    if (const auto selected = impl_->live_sessions->selected()) {
        try {
            impl_->sessions->validate(*selected);
            initial = *selected;
        } catch (const std::exception&) {
            initial = fallback_session(*impl_->sessions);
        }
    }
    result.presentation = make_bootstrap(
        *workspace,
        impl_->sessions->recent(),
        initial,
        std::move(vault.name),
        std::move(names));
    return result;
}

cha::web::CreateSessionSuccess Application::create_session(
    std::string_view forum_id,
    std::string label,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    try {
        if (!label.empty()) validate_session_label(label);
        const StoredSession created =
            impl_->sessions->create(forum_id, std::move(label));
        if (impl_->mirror) impl_->mirror->add(created);
        return {created.identity.session_id, created.label};
    } catch (const std::invalid_argument&) {
        throw ApplicationError(
            ErrorCode::invalid_argument, "Invalid session label.");
    }
}

std::variant<cha::web::OpenSessionSuccess, cha::web::ErrorCode>
Application::open_session(
    std::string_view forum_id,
    std::string_view session_id,
    std::uint64_t epoch) {
    const FullSessionId key{std::string(forum_id), std::string(session_id)};
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        if (const auto error = impl_->admit_locked(epoch)) return *error;
        try {
            impl_->sessions->validate(key);
        } catch (const SessionNotFoundError&) {
            return ErrorCode::not_found;
        } catch (const ForumNotFoundError&) {
            return ErrorCode::not_found;
        }
    }
    const auto outcome =
        impl_->live_sessions->select(key, impl_->settings.open_deadline);
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        if (const auto error = impl_->admit_locked(epoch)) {
            if (std::holds_alternative<cha::web::LiveSessionReady>(outcome)) {
                impl_->live_sessions->close_session(key);
            }
            return *error;
        }
    }
    if (std::holds_alternative<cha::web::LiveSessionReady>(outcome)) {
        return cha::web::OpenSessionSuccess{key.forum_id, key.session_id};
    }
    switch (std::get<cha::web::LiveSessionOpenFailure>(outcome)) {
    case cha::web::LiveSessionOpenFailure::not_found:
        return ErrorCode::not_found;
    case cha::web::LiveSessionOpenFailure::stopping:
        return ErrorCode::session_stopping;
    case cha::web::LiveSessionOpenFailure::limit_reached:
        return ErrorCode::session_limit_reached;
    case cha::web::LiveSessionOpenFailure::open_timeout:
        return ErrorCode::session_open_timeout;
    case cha::web::LiveSessionOpenFailure::manager_stopping:
        return ErrorCode::application_unavailable;
    case cha::web::LiveSessionOpenFailure::internal_error:
        return ErrorCode::internal_error;
    }
    return ErrorCode::internal_error;
}

cha::web::CommandSubmitResult Application::submit(
    std::string_view forum_id,
    std::string_view session_id,
    cha::web::WebCommand command,
    std::uint64_t epoch) {
    const FullSessionId key{std::string(forum_id), std::string(session_id)};
    std::optional<ErrorCode> denied;
    cha::web::LiveSessionHandle session;
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        denied = impl_->admit_locked(epoch);
        if (!denied) session = impl_->live_sessions->lookup(key);
    }
    if (denied) return *denied;
    if (!session) return ErrorCode::session_not_live;
    return session->submit(std::move(command), impl_->settings.command_deadline);
}

std::variant<std::shared_ptr<cha::web::CommandReply>, cha::web::ErrorCode>
Application::submit_async(
    std::string_view forum_id,
    std::string_view session_id,
    cha::web::WebCommand command,
    std::uint64_t epoch) {
    const FullSessionId key{std::string(forum_id), std::string(session_id)};
    if (epoch == 0) epoch = context_epoch();
    if (const auto denied = check_context(epoch)) return *denied;
    // The manager checks the epoch and maintenance gate with the lookup.
    // Stop must not wait for a transfer holding the application lifecycle lock.
    auto session = impl_->live_sessions->lookup(key, epoch);
    if (const auto denied = check_context(epoch)) return *denied;
    if (!session) return ErrorCode::session_not_live;
    return session->enqueue(std::move(command));
}

cha::web::CommandSubmitResult Application::stop(
    std::string_view forum_id,
    std::string_view session_id,
    std::uint64_t epoch) {
    return submit(forum_id, session_id, cha::web::StopCommand{}, epoch);
}

cha::web::CommandSubmitResult Application::snapshot(
    std::string_view forum_id,
    std::string_view session_id,
    std::uint64_t epoch) {
    const FullSessionId key{std::string(forum_id), std::string(session_id)};
    std::optional<ErrorCode> denied;
    cha::web::LiveSessionHandle session;
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        denied = impl_->admit_locked(epoch);
        if (!denied) session = impl_->live_sessions->lookup(key);
    }
    if (denied) return *denied;
    if (!session) return ErrorCode::session_not_live;
    return session->snapshot(impl_->settings.command_deadline);
}

cha::web::CommandSubmitResult Application::subscribe(
    std::string_view forum_id,
    std::string_view session_id,
    cha::web::SubscribeCommand command,
    std::uint64_t epoch) {
    const FullSessionId key{std::string(forum_id), std::string(session_id)};
    std::optional<ErrorCode> denied;
    cha::web::LiveSessionHandle session;
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        denied = impl_->admit_locked(epoch);
        if (!denied) session = impl_->live_sessions->lookup(key);
    }
    if (denied) return *denied;
    if (!session) return ErrorCode::session_not_live;
    return session->subscribe(std::move(command), impl_->settings.command_deadline);
}

cha::web::CommandSubmitResult Application::unsubscribe(
    std::string_view forum_id,
    std::string_view session_id,
    cha::web::UnsubscribeCommand command,
    std::uint64_t epoch) {
    const FullSessionId key{std::string(forum_id), std::string(session_id)};
    std::optional<ErrorCode> denied;
    cha::web::LiveSessionHandle session;
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        denied = impl_->admit_locked(epoch);
        if (!denied) session = impl_->live_sessions->lookup(key);
    }
    if (denied) return *denied;
    if (!session) return ErrorCode::session_not_live;
    return session->unsubscribe(std::move(command), impl_->settings.command_deadline);
}

void Application::close_session(
    std::string_view forum_id,
    std::string_view session_id,
    std::uint64_t epoch) {
    if (epoch == 0) epoch = context_epoch();
    if (check_context(epoch)) return;
    impl_->live_sessions->close_session(
        {std::string(forum_id), std::string(session_id)}, epoch);
}

std::optional<cha::web::ErrorCode> Application::delete_session(
    std::string_view forum_id,
    std::string_view session_id,
    std::uint64_t epoch) {
    const FullSessionId key{std::string(forum_id), std::string(session_id)};
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    if (const auto error = impl_->admit_locked(epoch)) return *error;
    if (workspace::is_welcome_session(key.forum_id, key.session_id)) {
        return ErrorCode::not_found;
    }
    cha::web::MaintenanceReservationResult reserved =
        impl_->live_sessions->reserve_for_deletion(
            key, impl_->settings.delete_deadline);
    if (const auto* failure =
            std::get_if<cha::web::MaintenanceFailure>(&reserved)) {
        return *failure == cha::web::MaintenanceFailure::manager_stopping
            ? cha::web::ErrorCode::application_unavailable
            : cha::web::ErrorCode::session_stopping;
    }
    try {
        impl_->sessions->delete_session(key);
    } catch (const SessionNotFoundError&) {
        return cha::web::ErrorCode::not_found;
    } catch (const ForumNotFoundError&) {
        return cha::web::ErrorCode::not_found;
    }
    impl_->media_resources.revoke_session(key);
    return std::nullopt;
}

std::vector<cha::web::SessionListing> Application::list_sessions(
    std::string_view forum_id,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    try {
        return workspace::sessions_for(
            *impl_->sessions, impl_->live_sessions->snapshot(), forum_id);
    } catch (const ForumNotFoundError&) {
        throw ApplicationError(ErrorCode::not_found);
    }
}

cha::web::SessionLabelResult Application::rename_session(
    std::string_view forum_id,
    std::string_view session_id,
    std::string label,
    std::uint64_t epoch) {
    const FullSessionId key{std::string(forum_id), std::string(session_id)};
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    if (workspace::is_welcome_session(key.forum_id, key.session_id)) {
        throw ApplicationError(ErrorCode::not_found);
    }
    try {
        validate_session_label(label);
    } catch (const std::invalid_argument&) {
        throw ApplicationError(
            ErrorCode::invalid_argument, "Invalid session label.");
    }
    const cha::web::LiveSessionHandle live = impl_->live_sessions->lookup(key);
    if (live) {
        const auto result = live->submit(
            cha::web::RenameSessionCommand{std::move(label)},
            impl_->settings.command_deadline);
        if (const auto* renamed =
                std::get_if<cha::web::SessionLabelResult>(&result)) {
            return *renamed;
        }
        if (const auto* error = std::get_if<ErrorCode>(&result)) {
            throw ApplicationError(*error);
        }
        throw ApplicationError(ErrorCode::internal_error);
    }
    try {
        const StoredSession renamed =
            impl_->sessions->rename(key, std::move(label));
        if (impl_->mirror) {
            impl_->mirror->update(
                renamed.identity,
                renamed.label,
                impl_->sessions->history(renamed.identity));
        }
        return {renamed.identity.session_id, renamed.label};
    } catch (const std::invalid_argument&) {
        throw ApplicationError(
            ErrorCode::invalid_argument, "Invalid session label.");
    }
}

cha::web::SessionExport Application::export_session(
    std::string_view forum_id,
    std::string_view session_id,
    std::uint64_t epoch) {
    const FullSessionId key{std::string(forum_id), std::string(session_id)};
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    const cha::web::LiveSessionHandle live = impl_->live_sessions->lookup(key);
    if (live) {
        const auto result = live->snapshot(impl_->settings.command_deadline);
        if (const auto* snapshot =
                std::get_if<cha::web::SessionSnapshot>(&result)) {
            return {cha::web::session_markdown(
                snapshot->session_label, snapshot->transcript)};
        }
        if (const auto* error = std::get_if<ErrorCode>(&result)) {
            throw ApplicationError(*error);
        }
        throw ApplicationError(ErrorCode::internal_error);
    }
    try {
        const PreparedSession prepared = impl_->sessions->prepare(key);
        return {cha::web::session_markdown(
            prepared.label, prepared.restore.entries)};
    } catch (const SessionNotFoundError&) {
        throw ApplicationError(ErrorCode::not_found);
    } catch (const ForumNotFoundError&) {
        throw ApplicationError(ErrorCode::not_found);
    }
}

cha::web::CharacterDetail Application::get_character(
    std::string_view character_id,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::get_character(character_id);
}

cha::web::CharacterDetail Application::create_character(
    cha::web::CreateCharacterRequest create,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::create_character(
        *impl_->store, create.display_name, create.description);
}

cha::web::CharacterDetail Application::update_character(
    std::string_view character_id,
    cha::web::CharacterSettingsUpdate update,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::update_character_settings(
        *impl_->store, *impl_->live_sessions, character_id, update);
}

cha::web::CharacterDetail Application::update_character_definition(
    std::string_view character_id,
    cha::web::CharacterDefinitionUpdate update,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::update_character_definition(
        *impl_->store, *impl_->live_sessions, character_id, update);
}

void Application::delete_character(
    std::string_view character_id,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    workspace::delete_character(*impl_->store, character_id);
}

cha::web::MarkdownFile Application::get_character_file(
    std::string_view character_id,
    std::string_view filename,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::get_character_file(character_id, filename);
}

cha::web::MarkdownFile Application::create_character_file(
    std::string_view character_id,
    std::string filename,
    std::string content,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::create_character_file(
        *impl_->store, *impl_->live_sessions, character_id,
        std::move(filename), std::move(content));
}

cha::web::MarkdownFile Application::update_character_file(
    std::string_view character_id,
    std::string filename,
    std::string content,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::update_character_file(
        *impl_->store, *impl_->live_sessions, character_id,
        std::move(filename), std::move(content));
}

void Application::delete_character_file(
    std::string_view character_id,
    std::string_view filename,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    workspace::delete_character_file(
        *impl_->store, *impl_->live_sessions, character_id, std::string(filename));
}

cha::web::PersonaDetail Application::get_persona(
    std::string_view persona_id,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::get_persona(persona_id);
}

cha::web::PersonaDetail Application::create_persona(
    std::string display_name,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::create_persona(*impl_->store, display_name);
}

cha::web::PersonaDetail Application::update_persona(
    std::string_view persona_id,
    cha::web::PersonaUpdate update,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::update_persona(
        *impl_->store, *impl_->live_sessions, persona_id, update);
}

void Application::delete_persona(
    std::string_view persona_id,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    workspace::delete_persona(*impl_->store, persona_id);
}

cha::web::ForumDetail Application::get_forum(
    std::string_view forum_id,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::get_forum(forum_id);
}

cha::web::ForumDetail Application::create_forum(
    cha::web::CreateForumRequest create,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::create_forum(
        *impl_->store, create.display_name, create.persona_id);
}

cha::web::ForumDetail Application::update_forum(
    std::string_view forum_id,
    cha::web::ForumUpdate update,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::update_forum(
        *impl_->store, *impl_->live_sessions, forum_id, update);
}

void Application::delete_forum(
    std::string_view forum_id,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    workspace::delete_forum(*impl_->store, *impl_->live_sessions, forum_id);
}

cha::web::ForumDetail Application::update_forum_members(
    std::string_view forum_id,
    cha::web::ForumMembersUpdate update,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::update_forum_members(
        *impl_->store, *impl_->live_sessions, forum_id, update);
}

cha::web::MarkdownFile Application::get_forum_file(
    std::string_view forum_id,
    std::string_view filename,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::get_forum_file(forum_id, filename);
}

cha::web::MarkdownFile Application::create_forum_file(
    std::string_view forum_id,
    std::string filename,
    std::string content,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::create_forum_file(
        *impl_->store, *impl_->live_sessions, forum_id,
        std::move(filename), std::move(content));
}

cha::web::MarkdownFile Application::update_forum_file(
    std::string_view forum_id,
    std::string filename,
    std::string content,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return workspace::update_forum_file(
        *impl_->store, *impl_->live_sessions, forum_id,
        std::move(filename), std::move(content));
}

void Application::delete_forum_file(
    std::string_view forum_id,
    std::string_view filename,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    workspace::delete_forum_file(
        *impl_->store, *impl_->live_sessions, forum_id, std::string(filename));
}

std::vector<cha::web::ProviderSummary> Application::list_providers(
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::list_providers();
}

cha::web::ProviderDetail Application::get_provider(
    std::string_view provider_id,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::get_provider(provider_id, *impl_->api_keys);
}

cha::web::ProviderDetail Application::create_provider(
    cha::web::CreateProviderRequest create,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::create_provider(*impl_->store, *impl_->api_keys, create);
}

cha::web::ProviderDetail Application::update_provider(
    std::string_view provider_id,
    nlohmann::json body,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::update_provider(
        *impl_->store,
        *impl_->live_sessions,
        *impl_->api_keys,
        provider_id,
        body);
}

void Application::delete_provider(
    std::string_view provider_id,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    settings::delete_provider(*impl_->store, provider_id);
}

std::shared_ptr<OperationReply> Application::test_provider(
    std::string_view provider_id,
    nlohmann::json body,
    std::uint64_t epoch) {
    auto reply = std::make_shared<OperationReply>();
    OpenAiOAuth* oauth = nullptr;
    ApiKeyStore* keys = nullptr;
    std::string id(provider_id);
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        impl_->require_admitted(epoch);
        (void)settings::get_provider(id, *impl_->api_keys);
        oauth = impl_->openai_auth.get();
        keys = impl_->api_keys.get();
    }
    if (!impl_->launch_background(
            [reply, id = std::move(id), body = std::move(body), oauth, keys](
                std::atomic_bool& cancel) {
                try {
                    settings::test_provider(id, body, *oauth, *keys, cancel);
                    if (cancel.load()) {
                        reply->fail(
                            ErrorCode::operation_cancelled,
                            "The operation was cancelled.");
                        return;
                    }
                    reply->complete(nlohmann::json::object());
                } catch (const ApplicationError& error) {
                    reply->fail(error.code, error.what());
                } catch (const std::exception& error) {
                    reply->fail(
                        ErrorCode::invalid_argument,
                        "Provider test failed: " + std::string(error.what()));
                } catch (...) {
                    reply->fail(ErrorCode::internal_error, {});
                }
            })) {
        reply->fail(
            ErrorCode::operation_cancelled, "The operation was cancelled.");
    }
    return reply;
}

std::vector<cha::web::StyleDetail> Application::list_styles(
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::list_styles();
}

cha::web::StyleDetail Application::create_style(
    std::string display_name,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::create_style(*impl_->store, display_name);
}

cha::web::StyleDetail Application::update_style(
    std::string_view style_id,
    cha::web::StyleUpdate update,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::update_style(
        *impl_->store, *impl_->live_sessions, style_id, update);
}

void Application::delete_style(std::string_view style_id, std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    settings::delete_style(*impl_->store, style_id);
}

std::vector<cha::web::VoiceDetail> Application::list_voices(
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::list_voices();
}

cha::web::VoiceDetail Application::create_voice(
    cha::web::CreateVoiceRequest create,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::create_voice(*impl_->store, create);
}

cha::web::VoiceDetail Application::update_voice(
    std::string_view voice_id,
    cha::web::VoiceUpdate update,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::update_voice(
        *impl_->store, *impl_->live_sessions, voice_id, update);
}

void Application::delete_voice(std::string_view voice_id, std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    settings::delete_voice(*impl_->store, voice_id);
}

std::optional<cha::web::VoiceInputSettings>
Application::get_voice_input_settings(std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::get_voice_input_settings();
}

cha::web::VoiceInputSettings Application::save_voice_input_settings(
    cha::web::VoiceInputSettings voice_settings,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::save_voice_input_settings(
        *impl_->store, *impl_->api_keys, voice_settings);
}

std::optional<cha::web::VoiceInputRuntime>
Application::get_voice_input_runtime(std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::get_voice_input_runtime(*impl_->api_keys, true);
}

std::optional<cha::web::VoiceOutputSettings>
Application::get_voice_output_settings(std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::get_voice_output_settings();
}

cha::web::VoiceOutputSettings Application::save_voice_output_settings(
    cha::web::VoiceOutputSettings voice_settings,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::save_voice_output_settings(
        *impl_->store, *impl_->api_keys, voice_settings);
}

std::optional<cha::web::VoiceOutputRuntime>
Application::get_voice_output_runtime(std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::get_voice_output_runtime(*impl_->api_keys, true);
}

std::shared_ptr<OperationReply> Application::start_speech(
    std::string_view connection_id,
    std::uint64_t request_id,
    std::string text,
    cha::web::FishAudioSynthesis synthesis,
    std::uint64_t epoch) {
    auto reply = std::make_shared<OperationReply>();
    WorkspaceVoiceOutput output;
    std::string key;
    cha::web::FishAudioRequest request;
    std::shared_ptr<Impl::PendingMedia> pending;
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        impl_->require_admitted(epoch);
        const auto workspace = getws();
        if (!workspace || !workspace->voice_output()
            || !impl_->api_keys->find(workspace->voice_output()->api_key_id)) {
            throw ApplicationError(
                ErrorCode::not_found, "Voice output is not configured.");
        }
        output = *workspace->voice_output();
        if (impl_->speech_url_override) output.url = *impl_->speech_url_override;
        if (!synthesis.reference_id) {
            const WorkspaceVoice* voice =
                workspace->find_voice_by_name(output.default_voice);
            if (!voice) {
                throw ApplicationError(
                    ErrorCode::not_found, "Voice output is not configured.");
            }
            synthesis.reference_id = voice->elevenlabs_voice_id;
        }
        key = impl_->api_keys->value(output.api_key_id);
        request = cha::web::make_fish_audio_request(output, text, synthesis);
        pending = impl_->remember_pending(std::string(connection_id), request_id);
    }
    if (!impl_->launch_background(
            [owner = impl_.get(), reply, pending, output = std::move(output),
             key = std::move(key), request = std::move(request), epoch](
                std::atomic_bool& cancel) {
                Impl::PendingMediaCleanup cleanup{owner, pending};
                const auto cancelled = [&] {
                    return cancel.load() || pending->cancelled->load();
                };
                try {
                    if (cancelled()) {
                        reply->fail(
                            ErrorCode::operation_cancelled,
                            "The operation was cancelled.");
                        return;
                    }
                    const auto transfer = owner->speech_proxy.synthesize(
                        output, key, request, cancelled);
                    if (cancelled() || transfer.cancelled) {
                        reply->fail(
                            ErrorCode::operation_cancelled,
                            "The operation was cancelled.");
                        return;
                    }
                    if (transfer.busy) {
                        reply->fail(
                            ErrorCode::speech_busy,
                            "Speech generation is busy. Try again shortly.");
                        return;
                    }
                    if (transfer.status != 200
                        || !cha::web::valid_entry_audio(transfer.audio)) {
                        throw_speech_provider_error(
                            transfer.status, transfer.audio.audio);
                    }
                    std::string id;
                    {
                        const std::lock_guard lifecycle(owner->lifecycle_mutex);
                        if (cancelled()) {
                            reply->fail(
                                ErrorCode::operation_cancelled,
                                "The operation was cancelled.");
                            return;
                        }
                        if (const auto error = owner->admit_locked(epoch)) {
                            reply->fail(*error, {});
                            return;
                        }
                        id = owner->media_resources.add(
                            pending->connection_id,
                            epoch,
                            ResourceKind::speech,
                            {transfer.audio.content_type, transfer.audio.audio});
                        if (!owner->set_pending_resource(pending, id)) {
                            (void)owner->media_resources.release(
                                pending->connection_id, id);
                            reply->fail(
                                ErrorCode::operation_cancelled,
                                "The operation was cancelled.");
                            return;
                        }
                    }
                    if (!reply->complete(media_resource_json(
                        id, transfer.audio.content_type,
                        transfer.audio.audio.size()))) {
                        owner->cancel_pending(
                            pending->connection_id, pending->request_id);
                    }
                } catch (const ApplicationError& error) {
                    reply->fail(error.code, error.what());
                } catch (const std::invalid_argument& error) {
                    reply->fail(ErrorCode::invalid_argument, error.what());
                } catch (const std::exception& error) {
                    log_warn(error.what());
                    reply->fail(
                        ErrorCode::internal_error, "FishAudio request failed.");
                } catch (...) {
                    reply->fail(ErrorCode::internal_error, {});
                }
            })) {
        impl_->forget_pending(pending);
        reply->fail(
            ErrorCode::operation_cancelled, "The operation was cancelled.");
    }
    return reply;
}

void Application::cancel_speech(
    std::string_view connection_id,
    std::uint64_t request_id,
    std::uint64_t epoch) {
    if (const auto denied = check_context(epoch)) throw ApplicationError(*denied);
    impl_->cancel_pending(connection_id, request_id);
}

void Application::release_resource(
    std::string_view connection_id,
    std::string_view resource_id,
    std::uint64_t epoch) {
    if (const auto denied = check_context(epoch)) throw ApplicationError(*denied);
    {
        std::lock_guard lock(impl_->media_mutex);
        std::erase_if(impl_->pending_media, [&](const auto& item) {
            return item.second->connection_id == connection_id
                && item.second->resource_id == resource_id;
        });
    }
    impl_->media_resources.release(connection_id, resource_id);
}

cha::web::AudioAcceptance Application::start_audio(
    std::string_view forum_id,
    std::string_view session_id,
    EntryId entry_id,
    cha::web::AudioDownloadRequest request,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    try {
        return impl_->audio_downloads->submit(
            {std::string(forum_id), std::string(session_id)},
            entry_id, request);
    } catch (const cha::web::AudioDownloadError& error) {
        throw_audio_error(error);
    }
}

std::vector<cha::web::AudioAcceptance> Application::start_audio_batch(
    std::string_view forum_id,
    std::string_view session_id,
    cha::web::AudioDownloadBatchRequest request,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    try {
        return impl_->audio_downloads->submit_batch(
            {std::string(forum_id), std::string(session_id)}, request);
    } catch (const cha::web::AudioDownloadError& error) {
        throw_audio_error(error);
    }
}

cha::web::AudioDownloadStatus Application::audio_status(
    std::string_view forum_id,
    std::string_view session_id,
    std::string_view vault_name,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    try {
        return impl_->audio_downloads->status(
            {std::string(forum_id), std::string(session_id)},
            std::string(vault_name));
    } catch (const cha::web::AudioDownloadError& error) {
        throw_audio_error(error);
    }
}

MediaResource Application::audio_source(
    std::string_view connection_id,
    std::string_view forum_id,
    std::string_view session_id,
    EntryId entry_id,
    std::string_view vault_name,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    std::optional<EntryAudio> audio;
    try {
        audio = impl_->audio_downloads->audio(
            {std::string(forum_id), std::string(session_id)},
            entry_id, std::string(vault_name));
    } catch (const cha::web::AudioDownloadError& error) {
        throw_audio_error(error);
    }
    if (!audio) {
        throw ApplicationError(ErrorCode::not_found, "Cached audio not found.");
    }
    const std::string id = impl_->media_resources.add(
        connection_id,
        epoch,
        ResourceKind::entry_audio,
        {audio->content_type, audio->audio},
        FullSessionId{std::string(forum_id), std::string(session_id)},
        entry_id);
    return {
        id,
        MediaResources::url_for(id),
        audio->content_type,
        audio->audio.size(),
    };
}

void Application::clear_audio_cache(
    std::string_view forum_id,
    std::string_view session_id,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    const FullSessionId session{std::string(forum_id), std::string(session_id)};
    try {
        impl_->audio_downloads->clear(session);
    } catch (const cha::web::AudioDownloadError& error) {
        throw_audio_error(error);
    }
    impl_->media_resources.revoke_session(session);
}

std::shared_ptr<OperationReply> Application::connect_voice_input(
    std::string_view connection_id,
    std::uint64_t request_id,
    std::string sdp,
    std::vector<std::string> languages,
    std::uint64_t epoch) {
    if (sdp.empty() || sdp.size() > 32768) {
        throw ApplicationError(
            ErrorCode::invalid_argument, "The request was not valid.");
    }
    if (languages.size() > 8) {
        throw ApplicationError(
            ErrorCode::invalid_argument, "The request was not valid.");
    }
    for (const std::string& language : languages) {
        if (language.empty() || language.size() > 16) {
            throw ApplicationError(
                ErrorCode::invalid_argument, "The request was not valid.");
        }
    }
    auto reply = std::make_shared<OperationReply>();
    std::string url;
    std::string key;
    std::string model;
    std::string delay;
    std::string prompt;
    std::shared_ptr<Impl::PendingMedia> pending;
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        impl_->require_admitted(epoch);
        const auto secret = settings::voice_input_secret(*impl_->api_keys, true);
        const auto runtime = settings::get_voice_input_runtime(
            *impl_->api_keys, true);
        if (!secret || !runtime) {
            throw ApplicationError(
                ErrorCode::not_found, "Voice input is not configured.");
        }
        url = runtime->url;
        key = *secret;
        model = runtime->model;
        delay = runtime->delay;
        prompt = runtime->prompt;
        pending = impl_->remember_pending(std::string(connection_id), request_id);
    }
    if (!impl_->launch_background(
            [owner = impl_.get(), reply, pending, url = std::move(url),
             key = std::move(key),
             model = std::move(model), delay = std::move(delay),
             prompt = std::move(prompt), sdp = std::move(sdp),
             languages = std::move(languages)](std::atomic_bool& cancel) {
                Impl::PendingMediaCleanup cleanup{owner, pending};
                const auto cancelled = [&] {
                    return cancel.load() || pending->cancelled->load();
                };
                try {
                    if (cancelled()) {
                        reply->fail(
                            ErrorCode::operation_cancelled,
                            "The operation was cancelled.");
                        return;
                    }
                    auto answer = connect_voice_transcription(
                        url, key, sdp, model, delay, prompt, languages,
                        cancelled);
                    if (!answer || cancelled()) {
                        reply->fail(
                            ErrorCode::operation_cancelled,
                            "The operation was cancelled.");
                        return;
                    }
                    reply->complete({{"sdp", std::move(*answer)}});
                } catch (const ApplicationError& error) {
                    reply->fail(error.code, error.what());
                } catch (const std::exception& error) {
                    log_warn(error.what());
                    reply->fail(
                        ErrorCode::internal_error,
                        "The realtime transcription request failed.");
                } catch (...) {
                    reply->fail(ErrorCode::internal_error, {});
                }
            })) {
        impl_->forget_pending(pending);
        reply->fail(
            ErrorCode::operation_cancelled, "The operation was cancelled.");
    }
    return reply;
}

void Application::cancel_voice_input(
    std::string_view connection_id,
    std::uint64_t request_id,
    std::uint64_t epoch) {
    if (const auto denied = check_context(epoch)) throw ApplicationError(*denied);
    impl_->cancel_pending(connection_id, request_id);
}

std::optional<ResourceBytes> Application::read_resource(
    std::string_view connection_id,
    std::string_view resource_id) const {
    const std::unique_lock lifecycle(
        impl_->lifecycle_mutex, std::try_to_lock);
    if (!lifecycle.owns_lock() || impl_->admit_locked(0)) return std::nullopt;
    return impl_->media_resources.read(
        connection_id, resource_id, impl_->published_epoch.load());
}

void Application::release_connection_resources(std::string_view connection_id) {
    impl_->cancel_connection_media(connection_id);
}

void Application::set_speech_url_override(std::string url) {
    impl_->speech_url_override = std::move(url);
}

std::vector<cha::web::ApiKeyDetail> Application::list_api_keys(
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::list_api_keys(*impl_->api_keys);
}

cha::web::ApiKeyDetail Application::create_api_key(
    cha::web::CreateApiKeyRequest create,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::create_api_key(*impl_->api_keys, create);
}

cha::web::ApiKeyDetail Application::rename_api_key(
    std::string_view api_key_id,
    std::string display_name,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::rename_api_key(*impl_->api_keys, api_key_id, display_name);
}

cha::web::ApiKeyDetail Application::replace_api_key_value(
    std::string_view api_key_id,
    std::string value,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::replace_api_key_value(*impl_->api_keys, api_key_id, value);
}

void Application::delete_api_key(
    std::string_view api_key_id,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    settings::delete_api_key(*impl_->api_keys, api_key_id);
}

std::optional<cha::web::R2StorageDetail> Application::get_r2_storage(
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::get_r2_storage(*impl_->api_keys);
}

cha::web::R2StorageDetail Application::save_r2_storage(
    cha::web::SaveR2StorageRequest request,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    auto saved = settings::save_r2_storage(*impl_->api_keys, request);
    impl_->publish_capabilities_locked();
    return saved;
}

void Application::delete_r2_storage(std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    settings::delete_r2_storage(*impl_->api_keys);
    impl_->publish_capabilities_locked();
}

cha::web::OpenAiAuth Application::openai_auth_status(std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::openai_auth_status(*impl_->openai_auth);
}

std::shared_ptr<OperationReply> Application::start_openai_auth(
    std::uint64_t epoch) {
    auto reply = std::make_shared<OperationReply>();
    OpenAiOAuth* oauth = nullptr;
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        impl_->require_admitted(epoch);
        oauth = impl_->openai_auth.get();
    }
    if (!impl_->launch_background(
            [reply, oauth](std::atomic_bool& cancel) {
                try {
                    const auto result =
                        settings::start_openai_auth(*oauth, cancel);
                    if (cancel.load()) {
                        reply->fail(
                            ErrorCode::operation_cancelled,
                            "The operation was cancelled.");
                        return;
                    }
                    reply->complete(result);
                } catch (const ApplicationError& error) {
                    reply->fail(error.code, error.what());
                } catch (...) {
                    reply->fail(ErrorCode::internal_error, {});
                }
            })) {
        reply->fail(
            ErrorCode::operation_cancelled, "The operation was cancelled.");
    }
    return reply;
}

std::shared_ptr<OperationReply> Application::poll_openai_auth(
    std::uint64_t epoch) {
    auto reply = std::make_shared<OperationReply>();
    OpenAiOAuth* oauth = nullptr;
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        impl_->require_admitted(epoch);
        oauth = impl_->openai_auth.get();
    }
    if (!impl_->launch_background(
            [reply, oauth](std::atomic_bool& cancel) {
                try {
                    const auto result =
                        settings::poll_openai_auth(*oauth, cancel);
                    if (cancel.load()) {
                        reply->fail(
                            ErrorCode::operation_cancelled,
                            "The operation was cancelled.");
                        return;
                    }
                    reply->complete(result);
                } catch (const ApplicationError& error) {
                    reply->fail(error.code, error.what());
                } catch (...) {
                    reply->fail(ErrorCode::internal_error, {});
                }
            })) {
        reply->fail(
            ErrorCode::operation_cancelled, "The operation was cancelled.");
    }
    return reply;
}

cha::web::OpenAiAuth Application::disconnect_openai_auth(std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    return settings::disconnect_openai_auth(*impl_->openai_auth);
}

std::optional<FullSessionId> Application::selected_session() const {
    return impl_->live_sessions->selected();
}

std::uint64_t Application::context_epoch() const {
    return impl_->published_epoch.load();
}

std::optional<cha::web::ErrorCode> Application::check_context(
    std::uint64_t epoch) const {
    const ApplicationState state = impl_->state.load();
    if (state == ApplicationState::maintenance) {
        return ErrorCode::vault_changed;
    }
    if (state != ApplicationState::running) {
        return ErrorCode::application_unavailable;
    }
    if (epoch != 0 && epoch != impl_->published_epoch.load()) {
        return ErrorCode::vault_changed;
    }
    return std::nullopt;
}

bool Application::running() const {
    return impl_->state.load() == ApplicationState::running;
}

ApplicationState Application::state() const {
    const ApplicationState state = impl_->state.load();
    if (state == ApplicationState::stopping) {
        return ApplicationState::unavailable;
    }
    return state;
}

ApplicationCapabilities Application::capabilities() const {
    return impl_->capabilities();
}

bool Application::has_r2_storage() const {
    return impl_->capabilities().can_transfer_r2;
}

void Application::set_resource_hooks(ResourceHooks hooks) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->resource_hooks = std::move(hooks);
}

void Application::set_context_changed(ContextChanged callback) {
    Impl::PendingContextNotice notice;
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        impl_->context_changed = std::move(callback);
        impl_->take_context_notice(notice);
    }
    notice.dispatch();
}

void Application::request_shutdown() {
    impl_->stopping_flag = true;
    impl_->state.store(ApplicationState::stopping);
    impl_->speech_proxy.stop();
    if (impl_->audio_downloads) impl_->audio_downloads->request_stop();
    impl_->pause_resources(true);
    impl_->live_sessions->begin_shutdown();
}

bool Application::join_shutdown(std::chrono::milliseconds grace) {
    const auto deadline = std::chrono::steady_clock::now() + grace;
    if (!impl_->join_background_until(deadline)) {
        impl_->state.store(ApplicationState::unavailable);
        impl_->preserve_on_destroy.store(true);
        return false;
    }
    std::unique_lock lifecycle(impl_->lifecycle_mutex, std::defer_lock);
    if (!lifecycle.try_lock_until(deadline)) {
        impl_->state.store(ApplicationState::unavailable);
        impl_->preserve_on_destroy.store(true);
        return false;
    }
    bool joined = true;
    if (impl_->audio_downloads) {
        impl_->audio_downloads->request_stop();
        joined = impl_->audio_downloads->join_until(deadline) && joined;
    }
    impl_->media_resources.revoke_all();
    const auto now = std::chrono::steady_clock::now();
    const auto remaining = now < deadline
        ? std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now)
        : std::chrono::milliseconds::zero();
    joined = impl_->live_sessions->join_shutdown(remaining) && joined;
    joined = impl_->providers.shutdown_until(deadline) && joined;
    impl_->stopped = true;
    impl_->running = false;
    impl_->state.store(ApplicationState::unavailable);
    impl_->preserve_on_destroy.store(!joined);
    return joined;
}

VaultRegistrySnapshot Application::vault_snapshot() const {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    return {impl_->command.vaults, impl_->current_vault_.get()};
}

VaultDefinition Application::create_vault(VaultCreate create, std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    const VaultDefinition* copied = nullptr;
    if (create.copy_from) {
        copied = find_vault(impl_->command.vaults, *create.copy_from);
        if (copied == nullptr) {
            throw std::invalid_argument("The source vault was not found");
        }
    }

    VaultDefinition candidate{
        .name = std::move(create.display_name),
        .password_protected = !create.password.empty(),
        .source = vault::next_vault_file(impl_->command.config_directory),
    };
    try {
        validate_public_name(candidate.name, "vault_name", candidate.source);
        require_path_component(candidate.name, candidate.source);
    } catch (const std::runtime_error& error) {
        throw std::invalid_argument(error.what());
    }
    candidate.data = vault::normalized_vault_path(
        impl_->command.config_directory,
        path_from_utf8(candidate.name + ".sqlite3"));
    vault::assign_vault_paths(candidate, impl_->command);
    vault::require_available_database_path(candidate.data);
    vault::validate_vault_paths(candidate);

    std::vector<VaultDefinition> updated = impl_->command.vaults;
    updated.push_back(candidate);
    vault::validate_candidate_vaults(impl_->command.config_directory, updated);

    write_toml_file(candidate.source, vault::vault_definition_table(candidate));
    try {
        if (copied != nullptr) {
            std::string source_password;
            if (copied->password_protected) {
                if (!same_vault_name(
                        copied->name, impl_->current_vault_.get().name)) {
                    throw std::invalid_argument(
                        "Switch to a protected source vault before copying it");
                }
                source_password = impl_->active_password;
            }
            copy_workspace_session_database(
                copied->data,
                candidate.data,
                source_password,
                create.password);
        } else {
            create_workspace_session_database_from_configuration(
                impl_->current_vault_.get().data,
                candidate.data,
                impl_->active_password,
                create.password);
        }
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(candidate.source, ignored);
        throw;
    }

    std::sort(
        updated.begin(), updated.end(),
        [](const VaultDefinition& left, const VaultDefinition& right) {
            return fold_ascii(left.name) < fold_ascii(right.name);
        });
    impl_->command.vaults = std::move(updated);
    impl_->publish_vault_names();
    return candidate;
}

VaultDefinition Application::update_vault(
    std::string_view current_name,
    VaultUpdate update,
    std::uint64_t epoch) {
    Impl::PendingContextNotice notice;
    VaultDefinition candidate_result;
    try {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        impl_->require_admitted(epoch);
        const auto found = std::find_if(
            impl_->command.vaults.begin(), impl_->command.vaults.end(),
            [current_name](const VaultDefinition& vault) {
                return same_vault_name(vault.name, current_name);
            });
        if (found == impl_->command.vaults.end()) {
            throw std::out_of_range("The vault was not found");
        }

        const VaultDefinition previous = *found;
        VaultDefinition candidate = previous;
        candidate.name = std::move(update.display_name);
        const bool enable_protection = !update.password.empty();
        if (enable_protection && previous.password_protected) {
            throw std::invalid_argument("This vault is already protected");
        }
        if (enable_protection) candidate.password_protected = true;
        vault::assign_vault_paths(candidate, impl_->command);
        vault::validate_vault_paths(candidate);

        std::vector<VaultDefinition> updated = impl_->command.vaults;
        updated[static_cast<std::size_t>(found - impl_->command.vaults.begin())] =
            candidate;
        vault::validate_candidate_vaults(impl_->command.config_directory, updated);

        const bool active = same_vault_name(
            impl_->current_vault_.get().name, previous.name);
        std::vector<vault::VaultDirectoryMove> moved;
        try {
            vault::move_vault_directory(
                previous.modify, candidate.modify, "modify", moved);
            vault::move_vault_directory(
                previous.mirror, candidate.mirror, "mirror", moved);
            write_toml_file(
                candidate.source, vault::vault_definition_table(candidate));
            if (active) {
                rewrite_toml_file(
                    impl_->command.config_directory / "app.toml",
                    [&](toml::table& table) {
                        table.insert_or_assign("vault", candidate.name);
                    });
            }
            if (enable_protection) {
                if (active) {
                    impl_->protect_active_database(update.password, notice);
                } else {
                    SessionLease lease = SessionLease::acquire(
                        previous.data,
                        "Database already in use: '"
                            + utf8_path(previous.data) + "'");
                    checkpoint_workspace_session_database(previous.data);
                    protect_workspace_session_database(
                        previous.data, update.password);
                }
            }
        } catch (...) {
            const bool protection_committed = enable_protection
                && inspect_workspace_session_database(
                       previous.data, update.password)
                    == WorkspaceDatabaseState::valid_v2;
            if (!protection_committed) {
                try {
                    write_toml_file(
                        previous.source,
                        vault::vault_definition_table(previous));
                } catch (const std::exception& error) {
                    log_critical(
                        "Failed to restore vault definition after update: "
                        + std::string(error.what()));
                }
                vault::restore_vault_directories(moved);
            }
            throw;
        }

        std::sort(
            updated.begin(), updated.end(),
            [](const VaultDefinition& left, const VaultDefinition& right) {
                return fold_ascii(left.name) < fold_ascii(right.name);
            });
        impl_->command.vaults = std::move(updated);
        if (active) {
            impl_->command.vault = candidate;
            impl_->publish_vault(candidate);
            impl_->publish_capabilities_locked();
            impl_->rebuild_mirror(cha::web::session_mirror_root(candidate));
        } else {
            impl_->publish_vault_names();
        }
        candidate_result = candidate;
    } catch (...) {
        notice.dispatch();
        throw;
    }
    notice.dispatch();
    return candidate_result;
}

void Application::delete_vault(std::string_view name, std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    const auto found = std::find_if(
        impl_->command.vaults.begin(), impl_->command.vaults.end(),
        [name](const VaultDefinition& vault) {
            return same_vault_name(vault.name, name);
        });
    if (found == impl_->command.vaults.end()) {
        throw std::out_of_range("The vault was not found");
    }
    if (impl_->command.vaults.size() == 1) {
        throw std::invalid_argument("The last vault cannot be deleted");
    }
    if (same_vault_name(impl_->current_vault_.get().name, found->name)) {
        throw std::invalid_argument("The active vault cannot be deleted");
    }
    (void)std::filesystem::remove(found->source);
    impl_->command.vaults.erase(found);
    impl_->publish_vault_names();
}

std::vector<std::string> Application::list_r2_vaults(std::uint64_t epoch) const {
    R2StorageKey key;
    std::vector<std::string> local_database_names;
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        impl_->require_admitted(epoch);
        const std::optional<R2StorageKey> configured = impl_->api_keys->r2();
        if (!configured) {
            throw std::invalid_argument(
                "R2 storage credentials are not configured for the active vault");
        }
        key = *configured;
        local_database_names.reserve(impl_->command.vaults.size());
        for (const VaultDefinition& vault : impl_->command.vaults) {
            local_database_names.push_back(utf8_path(vault.data.filename()));
        }
    }

    std::vector<std::string> names = cha::web::list_r2_database_names(
        key, [this] { return impl_->stopping_flag.load(); });
    std::erase_if(names, [&](const std::string& name) {
        const std::string database_name = name + ".sqlite3";
        return std::ranges::any_of(
            local_database_names,
            [&](const std::string& local) {
                return fold_ascii(local) == fold_ascii(database_name);
            });
    });
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        impl_->require_admitted(epoch);
    }
    return names;
}

VaultDefinition Application::download_r2_vault(
    std::string_view name,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    const std::optional<R2StorageKey> r2 = impl_->api_keys->r2();
    if (!r2) {
        throw std::invalid_argument(
            "R2 storage credentials are not configured for the active vault");
    }
    const std::string database_name = std::string(name) + ".sqlite3";
    require_path_component(database_name, impl_->command.config_directory);

    VaultDefinition candidate{
        .data = vault::normalized_vault_path(
            impl_->command.config_directory, path_from_utf8(database_name)),
        .source = vault::next_vault_file(impl_->command.config_directory),
    };
    vault::require_available_database_path(candidate.data);

    try {
        (void)cha::web::download_new_database_from_r2(
            candidate.data, candidate.source, database_name, *r2,
            [this] { return impl_->stopping_flag.load(); });
        candidate = load_vault_definition_file(
            impl_->command.config_directory, candidate.source);
        vault::assign_vault_paths(candidate, impl_->command);
        std::vector<VaultDefinition> updated = impl_->command.vaults;
        updated.push_back(candidate);
        vault::validate_candidate_vaults(impl_->command.config_directory, updated);
        std::sort(
            updated.begin(), updated.end(),
            [](const VaultDefinition& left, const VaultDefinition& right) {
                return fold_ascii(left.name) < fold_ascii(right.name);
            });
        impl_->command.vaults = std::move(updated);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(candidate.source, ignored);
        ignored.clear();
        std::filesystem::remove(candidate.data, ignored);
        throw;
    }

    impl_->publish_vault_names();
    return candidate;
}

MaintenanceResult Application::switch_vault(
    std::string_view name,
    std::string password,
    std::uint64_t epoch) {
    Impl::PendingContextNotice notice;
    MaintenanceResult result;
    try {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        impl_->require_admitted(epoch);
        const VaultDefinition* const target =
            find_vault(impl_->command.vaults, name);
        if (target == nullptr) {
            throw UnknownVaultError(
                "Unknown vault '" + std::string(name) + "'");
        }
        if (same_vault_name(impl_->current_vault_.get().name, target->name)) {
            impl_->take_context_notice(notice);
            result = {impl_->state.load(), impl_->published_epoch.load()};
        } else {
            if (target->password_protected && password.empty()) {
                throw VaultPasswordError(
                    "Password required to open this vault");
            }

            const VaultDefinition selected = *target;
            SessionLease target_lease = SessionLease::acquire(
                selected.data,
                "Database already in use: '" + utf8_path(selected.data) + "'");
            if (selected.password_protected) {
                require_openable_protected_database(selected.data, password);
            }
            require_switchable_database(selected.data, password);

            if (!impl_->drain_for_maintenance(notice)) {
                throw std::runtime_error(
                    "Could not pause active sessions for database maintenance");
            }
            bool database_closed = false;
            try {
                auto database = impl_->store->reserve_maintenance();
                SessionRepository::MaintenanceGuard repository =
                    impl_->sessions->reserve_maintenance();
                repository.checkpoint();
                database.close();
                database_closed = true;
                database.retarget(
                    selected.data, std::move(target_lease), password);
                repository.retarget(selected.data, password);
                impl_->active_password = std::move(password);
                impl_->reopen(database, repository);
                impl_->current_vault_.set(selected);
                impl_->command.vault = selected;
                try {
                    impl_->api_keys->migrate_vault();
                } catch (const std::exception& error) {
                    log_warn(
                        "Legacy API-key migration failed after switching vault: "
                        + std::string(error.what()));
                }
                impl_->end_maintenance_locked(true, notice);
            } catch (...) {
                if (impl_->unusable) {
                    impl_->global_maintenance.reset();
                    impl_->publish_epoch(
                        notice, ApplicationState::unavailable);
                } else if (
                    impl_->state.load() == ApplicationState::maintenance) {
                    if (database_closed) {
                        impl_->end_maintenance_locked(false, notice);
                    } else {
                        impl_->cancel_maintenance_locked(notice);
                    }
                }
                throw;
            }
            impl_->rebuild_mirror(cha::web::session_mirror_root(selected));
            try {
                rewrite_toml_file(
                    impl_->command.config_directory / "app.toml",
                    [&](toml::table& table) {
                        table.insert_or_assign("vault", selected.name);
                    });
            } catch (const std::exception& error) {
                log_warn(
                    "Failed to persist vault selection: "
                    + std::string(error.what()));
            }
            result = {impl_->state.load(), impl_->published_epoch.load()};
        }
    } catch (...) {
        notice.dispatch();
        throw;
    }
    notice.dispatch();
    return result;
}

MaintenanceResult Application::merge_vault(
    std::string_view source_name,
    std::string password,
    std::uint64_t epoch) {
    Impl::PendingContextNotice notice;
    MaintenanceResult result;
    try {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        impl_->require_admitted(epoch);
        const VaultDefinition* const source =
            find_vault(impl_->command.vaults, source_name);
        if (source == nullptr) {
            throw UnknownVaultError(
                "Unknown vault '" + std::string(source_name) + "'");
        }
        if (same_vault_name(impl_->current_vault_.get().name, source->name)) {
            throw std::invalid_argument("Cannot merge a vault into itself");
        }
        if (source->password_protected && password.empty()) {
            throw VaultPasswordError("Password required to open this vault");
        }

        const VaultDefinition selected = *source;
        SessionLease source_lease = SessionLease::acquire(
            selected.data,
            "Database already in use: '" + utf8_path(selected.data) + "'");
        if (selected.password_protected) {
            require_openable_protected_database(selected.data, password);
        }

        if (!impl_->drain_for_maintenance(notice)) {
            throw std::runtime_error(
                "Could not pause active sessions for database maintenance");
        }
        bool merged = false;
        try {
            impl_->store->merge(selected.data, source_lease, password);
            merged = true;
            {
                SessionRepository::MaintenanceGuard repository =
                    impl_->sessions->reserve_maintenance();
                repository.synchronize_forums(*current_workspace());
            }
            impl_->rebuild_mirror(
                cha::web::session_mirror_root(impl_->current_vault_.get()));
        } catch (const WorkspaceRestartRequiredError&) {
            if (impl_->state.load() == ApplicationState::maintenance) {
                impl_->end_maintenance_locked(false, notice);
            }
            throw;
        } catch (const std::exception& error) {
            if (impl_->state.load() == ApplicationState::maintenance) {
                if (!merged) {
                    impl_->cancel_maintenance_locked(notice);
                    throw;
                }
                impl_->end_maintenance_locked(false, notice);
            }
            throw WorkspaceRestartRequiredError(
                std::string(
                    "Configuration was committed but forums could not be "
                    "synchronized: ")
                + error.what() + ". Restart is required");
        } catch (...) {
            if (impl_->state.load() == ApplicationState::maintenance) {
                if (!merged) {
                    impl_->cancel_maintenance_locked(notice);
                    throw;
                }
                impl_->end_maintenance_locked(false, notice);
            }
            throw WorkspaceRestartRequiredError(
                "Configuration was committed but forums could not be "
                "synchronized. Restart is required");
        }
        impl_->end_maintenance_locked(true, notice);
        result = {impl_->state.load(), impl_->published_epoch.load()};
    } catch (...) {
        notice.dispatch();
        throw;
    }
    notice.dispatch();
    return result;
}

cha::web::R2DatabaseTransfer Application::upload_database() {
    return impl_->maintain_database([this] {
        const std::optional<R2StorageKey> r2 = impl_->api_keys->r2();
        if (!r2) throw std::runtime_error("The active vault has no R2 key");
        const VaultDefinition vault = impl_->current_vault_.get();
        return cha::web::upload_database_to_r2(
            vault.data,
            vault.source,
            *r2,
            cha::web::R2DatabaseLease::already_held,
            impl_->active_password, [this] { return impl_->stopping_flag.load(); });
    }, false);
}

cha::web::R2DatabaseTransfer Application::download_database() {
    std::optional<VaultDefinition> downloaded_vault;
    const cha::web::R2DatabaseTransfer result = impl_->maintain_database(
        [this, &downloaded_vault] {
            const std::optional<R2StorageKey> r2 = impl_->api_keys->r2();
            if (!r2) {
                throw std::runtime_error("The active vault has no R2 key");
            }
            const VaultDefinition vault = impl_->current_vault_.get();
            const cha::web::R2DatabaseTransfer transferred =
                cha::web::download_database_from_r2(
                    vault.data,
                    vault.source,
                    *r2,
                    cha::web::R2DatabaseLease::already_held,
                    impl_->active_password, [this] { return impl_->stopping_flag.load(); });
            downloaded_vault = load_vault_definition_file(
                impl_->command.config_directory, vault.source);
            vault::assign_vault_paths(*downloaded_vault, impl_->command);
            const auto configured = std::find_if(
                impl_->command.vaults.begin(), impl_->command.vaults.end(),
                [&](const VaultDefinition& candidate) {
                    return candidate.source == vault.source;
                });
            if (configured == impl_->command.vaults.end()) {
                throw std::runtime_error(
                    "The downloaded active vault is not in the vault registry");
            }
            *configured = *downloaded_vault;
            impl_->command.vault = *downloaded_vault;
            impl_->publish_vault(*downloaded_vault);
            return transferred;
        });
    if (!downloaded_vault) {
        throw std::logic_error("Downloaded vault definition was not published");
    }
    impl_->rebuild_mirror(cha::web::session_mirror_root(*downloaded_vault));
    return result;
}

WorkspaceConfigTransfer Application::import_configuration() {
    return impl_->maintain_database([this] {
        const VaultDefinition vault = impl_->current_vault_.get();
        if (!vault.modify) {
            throw std::runtime_error(
                "Application config requires 'modify' for Import");
        }
        return import_workspace_configuration(
            *vault.modify,
            vault.data,
            WorkspaceConfigLease::already_held,
            impl_->active_password);
    });
}

WorkspaceConfigTransfer Application::export_configuration() {
    return impl_->maintain_database([this] {
        const VaultDefinition vault = impl_->current_vault_.get();
        if (!vault.modify) {
            throw std::runtime_error(
                "Application config requires 'modify' for Export");
        }
        vault::clear_existing_export(*vault.modify);
        return export_workspace_configuration(
            vault.data,
            *vault.modify,
            WorkspaceConfigLease::already_held,
            impl_->active_password);
    }, false);
}

void Application::save_file(
    std::uint64_t epoch,
    const std::filesystem::path& destination,
    std::string_view contents) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    vault::save_file_replace(destination, contents);
}

cha::web::ApplicationCommand& Application::command() {
    return impl_->command;
}

const cha::web::ApplicationCommand& Application::command() const {
    return impl_->command;
}

const WebSettings& Application::settings() const {
    return impl_->settings;
}

cha::web::CurrentVault& Application::current_vault() {
    return impl_->current_vault_;
}

const cha::web::CurrentVault& Application::current_vault() const {
    return impl_->current_vault_;
}

WorkspaceConfigStore& Application::store() {
    return *impl_->store;
}

std::shared_ptr<SessionRepository> Application::sessions() {
    return impl_->sessions;
}

cha::web::LiveSessionManager& Application::live_sessions() {
    return *impl_->live_sessions;
}

cha::web::AudioDownloadManager& Application::audio_downloads() {
    return *impl_->audio_downloads;
}

cha::web::FishAudioProxy& Application::speech_proxy() {
    return impl_->speech_proxy;
}

Providers& Application::providers() {
    return impl_->providers;
}

ApiKeyStore& Application::api_keys() {
    return *impl_->api_keys;
}

OpenAiOAuth& Application::openai_auth() {
    return *impl_->openai_auth;
}

std::shared_ptr<cha::web::SessionMirror> Application::mirror() {
    return impl_->mirror;
}

std::string Application::active_password() const {
    return impl_->active_password;
}

std::string& Application::mutable_active_password() {
    return impl_->active_password;
}

void Application::set_active_password(std::string password) {
    impl_->active_password = std::move(password);
}

std::timed_mutex& Application::lifecycle_mutex() {
    return impl_->lifecycle_mutex;
}

bool Application::unusable() const {
    return impl_->unusable;
}

bool& Application::mutable_unusable() {
    return impl_->unusable;
}

void Application::mark_unusable() {
    impl_->unusable = true;
    impl_->state.store(ApplicationState::unavailable);
}

bool Application::stopping() const {
    return impl_->stopping_flag;
}

} // namespace cha::app
