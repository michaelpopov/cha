#include "app/application.h"

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
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
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
    WebSettings settings;
    settings.browser_disconnect_lifetime = false;
    settings.monotonic_event_sequence = true;
    return settings;
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
        running = true;
        notified_epoch = live_sessions->context_epoch();
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

    [[nodiscard]] std::optional<ErrorCode> admit_locked(std::uint64_t epoch) const {
        if (unusable || stopping_flag || stopped) {
            return ErrorCode::application_unavailable;
        }
        if (state == ApplicationState::maintenance) {
            return ErrorCode::vault_changed;
        }
        if (state != ApplicationState::running) {
            return ErrorCode::application_unavailable;
        }
        if (epoch != 0 && epoch != live_sessions->context_epoch()) {
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
        if (resource_hooks.pause) resource_hooks.pause(cancel);
    }

    void resume_resources() {
        if (unusable) return;
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
        const auto epoch = live_sessions->context_epoch();
        if (notified_epoch == epoch && notified_state == state) return;
        notified_epoch = epoch;
        notified_state = state;
        notice = {context_changed, epoch, state};
    }

    std::uint64_t publish_epoch(PendingContextNotice& notice) {
        const auto epoch = live_sessions->bump_context_epoch();
        take_context_notice(notice);
        return epoch;
    }

    // Caller holds lifecycle_mutex. False leaves the previous vault selected.
    bool drain_for_maintenance(bool cancel_audio = true) {
        if (unusable) {
            throw WorkspaceRestartRequiredError(
                "The workspace database could not be reopened after an earlier "
                "maintenance operation. Restart is required");
        }
        if (stopping_flag || stopped || state != ApplicationState::running) {
            throw std::runtime_error("CHA application is unavailable");
        }
        state = ApplicationState::maintenance;
        cha::web::GlobalMaintenanceResult reserved =
            live_sessions->reserve_global_maintenance(maintenance_grace());
        if (std::holds_alternative<cha::web::MaintenanceFailure>(reserved)) {
            state = ApplicationState::running;
            return false;
        }
        global_maintenance = std::move(
            std::get<cha::web::LiveSessionGlobalMaintenance>(reserved));
        pause_resources(cancel_audio);
        return true;
    }

    void end_maintenance_locked(bool available, PendingContextNotice& notice) {
        global_maintenance.reset();
        if (!available) {
            unusable = true;
            state = ApplicationState::unavailable;
            publish_epoch(notice);
            return;
        }
        state = ApplicationState::running;
        publish_epoch(notice);
        resume_resources();
    }

    void reopen(
        WorkspaceConfigStore::MaintenanceGuard& database,
        const SessionRepository::MaintenanceGuard& repository) {
        try {
            database.reopen();
            repository.synchronize_forums(*current_workspace());
        } catch (...) {
            unusable = true;
            state = ApplicationState::unavailable;
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
        if (!drain_for_maintenance()) {
            throw std::runtime_error(
                "Could not pause active sessions for database maintenance");
        }
        auto database = store->reserve_maintenance();
        SessionRepository::MaintenanceGuard repository =
            sessions->reserve_maintenance();
        const std::filesystem::path database_path = current_vault_.get().data;
        repository.checkpoint();
        database.close();
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
    }

    template<typename Operation>
    auto maintain_database(Operation operation, bool cancel_audio = true) {
        PendingContextNotice notice;
        using Result = decltype(operation());
        std::optional<Result> result;
        std::exception_ptr error;
        try {
            const std::lock_guard lifecycle(lifecycle_mutex);
            if (!drain_for_maintenance(cancel_audio)) {
                throw std::runtime_error(
                    "Could not pause active sessions for database maintenance");
            }
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
            notice.dispatch();
            throw;
        }
        notice.dispatch();
        if (error) std::rethrow_exception(error);
        return std::move(*result);
    }

    ~Impl() {
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
    mutable std::timed_mutex lifecycle_mutex;
    std::atomic_bool stopping_flag{};
    bool running{};
    bool stopped{};
    bool unusable{};
    ApplicationState state{ApplicationState::running};
    ResourceHooks resource_hooks;
    ContextChanged context_changed;
    std::uint64_t notified_epoch{};
    ApplicationState notified_state{ApplicationState::running};
    std::optional<cha::web::LiveSessionGlobalMaintenance> global_maintenance;
};

Application::Application(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

Application::~Application() = default;

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
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    ApplicationBootstrap result;
    result.context_epoch = impl_->live_sessions->context_epoch();
    result.state = impl_->unusable || impl_->stopping_flag || impl_->stopped
        ? ApplicationState::unavailable
        : impl_->state;
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
    std::optional<ErrorCode> denied;
    cha::web::LiveSessionHandle session;
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        denied = impl_->admit_locked(epoch);
        if (!denied) session = impl_->live_sessions->lookup(key);
    }
    if (denied) return *denied;
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
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        if (impl_->admit_locked(epoch)) return;
    }
    impl_->live_sessions->close_session(
        {std::string(forum_id), std::string(session_id)});
}

std::optional<cha::web::ErrorCode> Application::delete_session(
    std::string_view forum_id,
    std::string_view session_id,
    std::uint64_t epoch) {
    const FullSessionId key{std::string(forum_id), std::string(session_id)};
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        if (const auto error = impl_->admit_locked(epoch)) return *error;
        if (workspace::is_welcome_session(key.forum_id, key.session_id)) {
            return ErrorCode::not_found;
        }
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
    cha::web::LiveSessionHandle live;
    {
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
        live = impl_->live_sessions->lookup(key);
    }
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
    cha::web::LiveSessionHandle live;
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        impl_->require_admitted(epoch);
        live = impl_->live_sessions->lookup(key);
    }
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

std::optional<FullSessionId> Application::selected_session() const {
    return impl_->live_sessions->selected();
}

std::uint64_t Application::context_epoch() const {
    return impl_->live_sessions->context_epoch();
}

std::optional<cha::web::ErrorCode> Application::check_context(
    std::uint64_t epoch) const {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    return impl_->admit_locked(epoch);
}

bool Application::running() const {
    return impl_->running && !impl_->stopped && !impl_->stopping_flag
        && impl_->state == ApplicationState::running && !impl_->unusable;
}

ApplicationState Application::state() const {
    if (impl_->unusable || impl_->stopped || impl_->stopping_flag) {
        return ApplicationState::unavailable;
    }
    return impl_->state;
}

ApplicationCapabilities Application::capabilities() const {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    const VaultDefinition vault = impl_->current_vault_.get();
    return {
        .can_modify = static_cast<bool>(vault.modify) && impl_->state == ApplicationState::running,
        .can_transfer_r2 = impl_->api_keys->r2().has_value()
            && impl_->state == ApplicationState::running,
    };
}

bool Application::has_r2_storage() const {
    return impl_->api_keys->r2().has_value();
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
    impl_->state = ApplicationState::stopping;
    impl_->live_sessions->begin_shutdown();
}

bool Application::join_shutdown(std::chrono::milliseconds grace) {
    const bool joined = impl_->live_sessions->join_shutdown(grace);
    impl_->providers.shutdown();
    impl_->stopped = true;
    impl_->running = false;
    impl_->state = ApplicationState::unavailable;
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

    std::vector<std::string> names = cha::web::list_r2_database_names(key);
    std::erase_if(names, [&](const std::string& name) {
        const std::string database_name = name + ".sqlite3";
        return std::ranges::any_of(
            local_database_names,
            [&](const std::string& local) {
                return fold_ascii(local) == fold_ascii(database_name);
            });
    });
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
            candidate.data, candidate.source, database_name, *r2);
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
            result = {impl_->state, impl_->live_sessions->context_epoch()};
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

            if (!impl_->drain_for_maintenance()) {
                throw std::runtime_error(
                    "Could not pause active sessions for database maintenance");
            }
            try {
                auto database = impl_->store->reserve_maintenance();
                SessionRepository::MaintenanceGuard repository =
                    impl_->sessions->reserve_maintenance();
                repository.checkpoint();
                database.close();
                database.retarget(
                    selected.data, std::move(target_lease), password);
                repository.retarget(selected.data, password);
                impl_->active_password = std::move(password);
                impl_->reopen(database, repository);
                impl_->current_vault_.set(selected);
                impl_->command.vault = selected;
                impl_->end_maintenance_locked(true, notice);
            } catch (...) {
                impl_->global_maintenance.reset();
                if (impl_->unusable) {
                    impl_->state = ApplicationState::unavailable;
                    impl_->publish_epoch(notice);
                } else if (impl_->state == ApplicationState::maintenance) {
                    impl_->state = ApplicationState::running;
                    impl_->resume_resources();
                }
                throw;
            }
            try {
                impl_->api_keys->migrate_vault();
            } catch (const std::exception& error) {
                log_warn(
                    "Legacy API-key migration failed after switching vault: "
                    + std::string(error.what()));
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
            result = {impl_->state, impl_->live_sessions->context_epoch()};
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

        try {
            impl_->store->merge(selected.data, source_lease, password);
        } catch (const WorkspaceRestartRequiredError&) {
            impl_->unusable = true;
            impl_->state = ApplicationState::unavailable;
            impl_->publish_epoch(notice);
            throw;
        }

        try {
            impl_->sessions->synchronize_forums();
        } catch (const std::exception& error) {
            impl_->unusable = true;
            impl_->state = ApplicationState::unavailable;
            impl_->publish_epoch(notice);
            throw WorkspaceRestartRequiredError(
                std::string(
                    "Configuration was committed but forums could not be "
                    "synchronized: ")
                + error.what() + ". Restart is required");
        }

        for (const cha::web::LiveSessionHandle& live :
             impl_->live_sessions->active_sessions()) {
            live->request_shutdown(cha::web::ShutdownReason::reloading);
        }
        impl_->publish_epoch(notice);
        impl_->rebuild_mirror(
            cha::web::session_mirror_root(impl_->current_vault_.get()));
        result = {impl_->state, impl_->live_sessions->context_epoch()};
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
            impl_->active_password);
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
                    impl_->active_password);
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
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        impl_->require_admitted(epoch);
    }
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
}

bool Application::stopping() const {
    return impl_->stopping_flag;
}

} // namespace cha::app
