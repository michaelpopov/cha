#include "web/application_runtime.h"

#include "providers/openai_oauth.h"
#include "providers/api_key_store.h"
#include "providers/provider_client.h"
#include "providers/providers.h"
#include "session/session_lease.h"
#include "session/session_repository.h"
#include "session/sqlite_storage.h"
#include "session/workspace_session_database.h"
#include "util/environment.h"
#include "util/logging.h"
#include "util/path_name.h"
#include "util/public_name.h"
#include "util/text.h"
#include "util/toml_file.h"
#include "web/current_vault.h"
#include "workspace/builtins.h"
#include "workspace/session_open.h"
#include "workspace/workspace.h"
#include "workspace/workspace_config_store.h"
#include "web/asset_handler.h"
#include "web/http_server.h"
#include "web/fish_audio.h"
#include "web/audio_download.h"
#include "web/audio_download_routes.h"
#include "web/json.h"
#include "web/live_session_manager.h"
#include "web/lobby_routes.h"
#include "web/openai_auth_routes.h"
#include "web/server_shutdown.h"
#include "web/session_mirror.h"
#include "web/session_routes.h"
#include "web/settings_routes.h"
#include "web/vault_routes.h"
#include "web/web_settings.h"

#include <httplib.h>
#include <toml++/toml.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace cha::web {
namespace {

void configure_test_idle_grace(
    WebSettings& settings,
    const ApplicationCommand& command) {
    if (command.test_idle_grace_ms) {
        settings.idle_grace =
            std::chrono::milliseconds(*command.test_idle_grace_ms);
        settings.orphan_limit = std::max(
            settings.orphan_limit, settings.idle_grace);
        const auto max_interval = std::max(
            std::chrono::milliseconds{1}, settings.idle_grace / 2);
        settings.sse_heartbeat_interval = std::min(
            settings.sse_heartbeat_interval, max_interval);
    }
    if (command.test_shutdown_grace_ms) {
        settings.shutdown_grace =
            std::chrono::milliseconds(*command.test_shutdown_grace_ms);
    }
}

void require_switchable_database(
    const std::filesystem::path& database,
    std::string_view password = {}) {
    const WorkspaceDatabaseState state =
        inspect_workspace_session_database(database, password);
    if (state == WorkspaceDatabaseState::valid_v2) return;
    if (state == WorkspaceDatabaseState::missing) {
        throw std::runtime_error(
            "Workspace session database '" + utf8_path(database)
            + "' does not exist");
    }
    if (state == WorkspaceDatabaseState::valid_v1) {
        throw std::runtime_error(
            "Workspace session database '" + utf8_path(database)
            + "' is a valid CHA schema-1 database");
    }
    if (state == WorkspaceDatabaseState::wrong_application_id
        || state == WorkspaceDatabaseState::unsupported_version) {
        throw std::runtime_error(
            "Workspace session database '" + utf8_path(database)
            + "' has an unsupported schema");
    }
    throw std::runtime_error(
        "Workspace session database '" + utf8_path(database)
        + "' is not a valid CHA database");
}

void require_openable_protected_database(
    const std::filesystem::path& database,
    std::string_view password) {
    const WorkspaceDatabaseState state =
        inspect_workspace_session_database(database, password);
    if (state == WorkspaceDatabaseState::valid_v2) return;
    if (state != WorkspaceDatabaseState::corrupt) {
        require_switchable_database(database, password);
    }

    const WorkspaceDatabaseState without_password =
        inspect_workspace_session_database(database);
    if (without_password == WorkspaceDatabaseState::valid_v2) {
        throw std::runtime_error(
            "Vault '" + utf8_path(database)
            + "' is marked as protected, but its database is not encrypted");
    }
    if (without_password != WorkspaceDatabaseState::corrupt) {
        require_switchable_database(database);
    }
    try {
        storage::SqliteDatabase handle(
            database, storage::SqliteDatabase::Mode::read_only, password);
        (void)handle.pragma_integer("application_id");
        (void)handle.pragma_integer("user_version");
    } catch (const std::runtime_error&) {
        throw VaultPasswordError(
            "The vault password is incorrect, or its database is damaged");
    }
    require_switchable_database(database, password);
}

void log_startup(const WebSettings& settings) {
    log_info(
        "web server event=startup session_limit="
        + std::to_string(settings.session_limit)
        + " http_thread_pool_size="
        + std::to_string(settings.http_thread_pool_size)
        + " http_pending_request_limit="
        + std::to_string(settings.http_pending_request_limit)
        + " command_queue_capacity="
        + std::to_string(settings.command_queue_capacity)
        + " request_body_limit="
        + std::to_string(settings.request_body_limit)
        + " prompt_limit=" + std::to_string(settings.prompt_limit)
        + " http_read_timeout_ms="
        + std::to_string(settings.http_read_timeout.count())
        + " http_write_timeout_ms="
        + std::to_string(settings.http_write_timeout.count())
        + " open_deadline_ms="
        + std::to_string(settings.open_deadline.count())
        + " command_deadline_ms="
        + std::to_string(settings.command_deadline.count()));
}

bool cookie_matches(
    const httplib::Request& request,
    std::string_view expected_token) {
    const std::string header = request.get_header_value("Cookie");
    std::string_view remaining(header);
    while (!remaining.empty()) {
        const std::size_t end = remaining.find(';');
        std::string_view cookie = remaining.substr(0, end);
        while (!cookie.empty() && cookie.front() == ' ') {
            cookie.remove_prefix(1);
        }
        constexpr std::string_view prefix = "CHA_RUNTIME=";
        if (cookie.starts_with(prefix)
            && cookie.substr(prefix.size()) == expected_token) {
            return true;
        }
        if (end == std::string_view::npos) break;
        remaining.remove_prefix(end + 1);
    }
    return false;
}

std::shared_ptr<const Workspace> current_workspace() {
    std::shared_ptr<const Workspace> workspace = getws();
    if (!workspace) throw std::runtime_error("Workspace is not loaded");
    return workspace;
}

ProviderClientFactory shared_openai_provider_factory(
    OpenAiOAuth* oauth,
    ApiKeyStore* api_keys) {
    return [oauth, api_keys](SharedCharacterDefinition definition) {
        return std::make_unique<ProviderClient>(
            std::move(definition), oauth, api_keys);
    };
}

std::filesystem::path normalized_vault_path(
    const std::filesystem::path& config_directory,
    const std::filesystem::path& value) {
    if (value.empty()) throw std::invalid_argument("A vault path is empty");
    const std::filesystem::path resolved = value.is_relative()
        ? config_directory / value : value;
    return std::filesystem::weakly_canonical(
        std::filesystem::absolute(resolved));
}

toml::table vault_definition_table(const VaultDefinition& vault) {
    toml::table table;
    table.insert("vault_name", vault.name);
    table.insert("data", utf8_path(vault.data));
    table.insert("protected", vault.password_protected);
    return table;
}

std::optional<std::filesystem::path> vault_path(
    const std::optional<std::filesystem::path>& base,
    std::string_view vault_name) {
    if (!base) return std::nullopt;
    return std::filesystem::weakly_canonical(
        *base / path_from_utf8(vault_name));
}

void assign_vault_paths(
    VaultDefinition& vault,
    const ApplicationCommand& command) {
    vault.mirror = vault_path(command.mirror_base, vault.name);
    vault.modify = vault_path(command.modify_base, vault.name);
}

std::optional<std::filesystem::path> session_mirror_root(
    const VaultDefinition& vault) {
    if (vault.password_protected) {
        if (vault.mirror) {
            log_warn("Ignoring session mirror for password-protected vault '"
                + vault.name + "'");
        }
        return std::nullopt;
    }
    return vault.mirror;
}

using VaultDirectoryMove =
    std::pair<std::filesystem::path, std::filesystem::path>;

void move_vault_directory(
    const std::optional<std::filesystem::path>& from,
    const std::optional<std::filesystem::path>& to,
    std::string_view field,
    std::vector<VaultDirectoryMove>& moved) {
    if (!from || !to || *from == *to || !std::filesystem::exists(*from)) return;
    if (std::filesystem::exists(*to)) {
        if (std::filesystem::equivalent(*from, *to)) return;
        throw std::invalid_argument(
            "The " + std::string(field)
            + " directory for the renamed vault already exists");
    }
    std::filesystem::rename(*from, *to);
    moved.emplace_back(*from, *to);
}

void restore_vault_directories(
    const std::vector<VaultDirectoryMove>& moved) noexcept {
    for (auto entry = moved.rbegin(); entry != moved.rend(); ++entry) {
        std::error_code error;
        std::filesystem::rename(entry->second, entry->first, error);
        if (error) {
            log_critical(
                "Failed to restore vault directory '"
                + utf8_path(entry->second) + "' to '"
                + utf8_path(entry->first) + "': " + error.message());
        }
    }
}

std::filesystem::path next_vault_file(
    const std::filesystem::path& config_directory) {
    for (std::size_t suffix = 1;; ++suffix) {
        const std::filesystem::path candidate =
            config_directory / ("vault-" + std::to_string(suffix) + ".toml");
        if (!std::filesystem::exists(candidate)) return candidate;
    }
}

void validate_candidate_vaults(
    const std::filesystem::path& config_directory,
    const std::vector<VaultDefinition>& vaults) {
    try {
        for (const VaultDefinition& vault : vaults) {
            validate_public_name(vault.name, "vault_name", vault.source);
            require_path_component(vault.name, vault.source);
        }
        validate_vault_definitions(config_directory, vaults);
    } catch (const std::runtime_error& error) {
        throw std::invalid_argument(error.what());
    }
}

bool is_workspace_directory(const std::filesystem::path& path) {
    try {
        (void)Workspace::load(path);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void validate_vault_paths(const VaultDefinition& vault) {
    if (!vault.password_protected && vault.mirror
        && std::filesystem::exists(*vault.mirror)
        && !std::filesystem::is_directory(*vault.mirror)) {
        throw std::invalid_argument(
            "The mirror path must be an existing directory");
    }
    if (!vault.modify || !std::filesystem::exists(*vault.modify)) return;
    if (!std::filesystem::is_directory(*vault.modify)) {
        throw std::invalid_argument(
            "The modify path must be a directory");
    }
    if (std::filesystem::is_empty(*vault.modify)) return;
    if (!is_workspace_directory(*vault.modify)) {
        throw std::invalid_argument(
            "The modify path must be empty or contain a valid CHA workspace");
    }
}

void require_available_database_path(const std::filesystem::path& data) {
    if (!std::filesystem::is_directory(data.parent_path())) {
        throw std::invalid_argument("The database parent directory does not exist");
    }
    std::error_code status_error;
    const std::filesystem::file_status data_status =
        std::filesystem::symlink_status(data, status_error);
    if (status_error && status_error != std::errc::no_such_file_or_directory) {
        throw std::runtime_error("The database path could not be inspected");
    }
    if (data_status.type() != std::filesystem::file_type::not_found) {
        throw std::invalid_argument("The database path already exists");
    }
}

void clear_existing_export(const std::filesystem::path& destination) {
    if (!std::filesystem::is_directory(destination)
        || std::filesystem::is_empty(destination)) {
        return;
    }
    if (!is_workspace_directory(destination)) {
        throw std::runtime_error(
            "Export destination '" + utf8_path(destination)
            + "' is not a valid CHA workspace; refusing to replace it");
    }
    std::filesystem::remove_all(destination);
}

} // namespace

struct ApplicationRuntime::Impl {
    explicit Impl(
        const ApplicationCommand& selected_command,
        std::string selected_access_token,
        std::string selected_vault_password)
        : command(selected_command),
          access_token(std::move(selected_access_token)),
          active_password(std::move(selected_vault_password)),
          settings(),
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
        configure_test_idle_grace(settings, command);
        const auto seed = TemporarySessionSeed{
            {std::string(entrance_id), std::string(welcome_id)},
            std::string(welcome_name)};
        sessions = std::make_shared<SessionRepository>(
            store->database_path(),
            store->workspace_path(),
            store->welcome_path(),
            seed,
            active_password);
        audio_downloads = std::make_unique<AudioDownloadManager>(*sessions, current_vault_, !access_token.empty());
        mirror = std::make_shared<SessionMirror>();
        if (const auto root = session_mirror_root(command.vault)) {
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
            OpenedSession opened = open_session(
                *sessions, identity, providers, std::move(notifier), *store);
            const auto selected_mirror = mirror;
            opened.mirror = [selected_mirror, identity](
                                std::string_view label,
                                std::span<const TranscriptEntry> entries) {
                selected_mirror->update(identity, label, entries);
            };
            return opened;
        };
        live_sessions = std::make_unique<LiveSessionManager>(settings, opener);
    }

    void publish_vault_names() {
        std::vector<std::string> names;
        names.reserve(command.vaults.size());
        for (const VaultDefinition& vault : command.vaults) {
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

    // shutdown() covers the running case; this covers the paths that never got
    // there, such as a start() that failed after the providers were built.
    ~Impl() { providers.shutdown(); }

    struct AudioMaintenance {
        Impl& runtime;
        explicit AudioMaintenance(Impl& runtime, bool cancel = true) : runtime(runtime) { runtime.audio_downloads->pause(cancel); }
        ~AudioMaintenance() { if (!runtime.unusable) runtime.audio_downloads->resume(); }
    };

    template<typename Operation>
    auto maintain_database(Operation operation, bool cancel_audio = true) {
        const std::lock_guard lifecycle(lifecycle_mutex);
        if (unusable) {
            throw WorkspaceRestartRequiredError(
                "The workspace database could not be reopened after an earlier "
                "maintenance operation. Restart is required");
        }
        if (!started || stopped) {
            throw std::runtime_error("CHA runtime is not running");
        }

        GlobalMaintenanceResult reserved =
            live_sessions->reserve_global_maintenance(settings.shutdown_grace);
        if (std::holds_alternative<MaintenanceFailure>(reserved)) {
            throw std::runtime_error(
                "Could not pause active sessions for database maintenance");
        }
        auto global = std::move(
            std::get<LiveSessionGlobalMaintenance>(reserved));
        AudioMaintenance audio(*this, cancel_audio);
        // The store guard comes first because it holds the configuration
        // mutex: without it a configuration edit can still be inside its own
        // SQLite transaction, and the checkpoint below would report busy.
        auto database = store->reserve_maintenance();
        SessionRepository::MaintenanceGuard repository =
            sessions->reserve_maintenance();
        repository.checkpoint();
        database.close();
        bool reopened = false;

        try {
            auto result = operation();
            reopen(database, repository);
            reopened = true;
            return result;
        } catch (...) {
            if (!reopened) reopen_after_failure(database, repository);
            throw;
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
            stop_http();
            throw;
        }
    }

    // Reopening on the failure path must not hide why the transfer failed,
    // but a store that cannot be reopened outranks it: the process can no
    // longer serve, so that failure is the one the caller sees.
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

    void protect_active_database(std::string password) {
        GlobalMaintenanceResult reserved =
            live_sessions->reserve_global_maintenance(settings.shutdown_grace);
        if (std::holds_alternative<MaintenanceFailure>(reserved)) {
            throw std::runtime_error(
                "Could not pause active sessions for database maintenance");
        }
        auto global = std::move(
            std::get<LiveSessionGlobalMaintenance>(reserved));
        AudioMaintenance audio(*this);
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
            reopen_after_failure(database, repository);
            throw;
        }
        mirror->rebuild(std::nullopt, *sessions);
        active_password = std::move(password);
        database.set_password(active_password);
        repository.retarget(database_path, active_password);
        reopen(database, repository);
    }

    ApplicationCommand command;
    std::string access_token;
    std::string active_password;
    WebSettings settings;
    CurrentVault current_vault_;
    std::unique_ptr<WorkspaceConfigStore> store;
    std::shared_ptr<SessionRepository> sessions;
    std::shared_ptr<SessionMirror> mirror;
    std::unique_ptr<ApiKeyStore> api_keys;
    std::unique_ptr<OpenAiOAuth> openai_auth;
    Providers providers;
    std::unique_ptr<LiveSessionManager> live_sessions;
    FishAudioProxy fish_audio;
    std::unique_ptr<AudioDownloadManager> audio_downloads;
    std::unique_ptr<httplib::Server> server;
    std::thread listener;
    mutable std::timed_mutex lifecycle_mutex;
    std::atomic_bool stopping{};
    bool started{};
    bool stopped{};
    // Set when the workspace database could not be reopened. The HTTP server is
    // stopped at the same time because the runtime can no longer serve safely.
    bool unusable{};
    bool server_stop_requested{};

    void stop_http() {
        if (audio_downloads) audio_downloads->request_stop();
        if (!server || server_stop_requested) return;
        server_stop_requested = true;
        fish_audio.stop();
        server->stop();
    }
};

ApplicationRuntime::ApplicationRuntime(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

ApplicationRuntime::~ApplicationRuntime() {
    if (impl_ && impl_->started && !impl_->stopped) shutdown();
}

std::unique_ptr<ApplicationRuntime> ApplicationRuntime::open(
    const ApplicationCommand& command,
    std::string access_token,
    std::string vault_password) {
    for (const std::string& warning : command.warnings) log_warn(warning);
    load_dotenv(command.config_directory / ".env");
    if (command.vault.password_protected && vault_password.empty()) {
        throw VaultPasswordError("Password required to open this vault");
    }
    if (command.vault.password_protected) {
        require_openable_protected_database(
            command.vault.data, vault_password);
    }
    return std::unique_ptr<ApplicationRuntime>(new ApplicationRuntime(
        std::make_unique<Impl>(
            command, std::move(access_token), std::move(vault_password))));
}

VaultDefinition ApplicationRuntime::current_vault() const {
    return impl_->current_vault_.get();
}

VaultRegistrySnapshot ApplicationRuntime::vault_snapshot() const {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    return {impl_->command.vaults, impl_->current_vault_.get()};
}

std::string ApplicationRuntime::api_key_value(std::string_view id) const {
    return impl_->api_keys->value(id);
}

bool ApplicationRuntime::has_r2_storage() const {
    return impl_->api_keys->r2().has_value();
}

VaultDefinition ApplicationRuntime::create_vault(VaultCreate create) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
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
        .source = next_vault_file(impl_->command.config_directory),
    };
    try {
        validate_public_name(candidate.name, "vault_name", candidate.source);
        require_path_component(candidate.name, candidate.source);
    } catch (const std::runtime_error& error) {
        throw std::invalid_argument(error.what());
    }
    candidate.data = normalized_vault_path(
        impl_->command.config_directory,
        path_from_utf8(candidate.name + ".sqlite3"));
    assign_vault_paths(candidate, impl_->command);
    require_available_database_path(candidate.data);
    validate_vault_paths(candidate);

    std::vector<VaultDefinition> updated = impl_->command.vaults;
    updated.push_back(candidate);
    validate_candidate_vaults(impl_->command.config_directory, updated);

    write_toml_file(candidate.source, vault_definition_table(candidate));
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

VaultDefinition ApplicationRuntime::update_vault(
    std::string_view current_name,
    VaultUpdate update) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
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
    assign_vault_paths(candidate, impl_->command);
    validate_vault_paths(candidate);

    std::vector<VaultDefinition> updated = impl_->command.vaults;
    updated[static_cast<std::size_t>(found - impl_->command.vaults.begin())] =
        candidate;
    validate_candidate_vaults(impl_->command.config_directory, updated);

    const bool active = same_vault_name(
        impl_->current_vault_.get().name, previous.name);
    std::vector<VaultDirectoryMove> moved;
    try {
        move_vault_directory(
            previous.modify, candidate.modify, "modify", moved);
        move_vault_directory(
            previous.mirror, candidate.mirror, "mirror", moved);
        write_toml_file(candidate.source, vault_definition_table(candidate));
        if (active) {
            rewrite_toml_file(
                impl_->command.config_directory / "app.toml",
                [&](toml::table& table) {
                    table.insert_or_assign("vault", candidate.name);
                });
        }
        if (enable_protection) {
            if (active) {
                impl_->protect_active_database(update.password);
            } else {
                SessionLease lease = SessionLease::acquire(
                    previous.data,
                    "Database already in use: '" + utf8_path(previous.data) + "'");
                checkpoint_workspace_session_database(previous.data);
                protect_workspace_session_database(previous.data, update.password);
            }
        }
    } catch (...) {
        const bool protection_committed = enable_protection
            && inspect_workspace_session_database(previous.data, update.password)
                == WorkspaceDatabaseState::valid_v2;
        if (!protection_committed) {
            try {
                write_toml_file(previous.source, vault_definition_table(previous));
            } catch (const std::exception& error) {
                log_critical(
                    "Failed to restore vault definition after update: "
                    + std::string(error.what()));
            }
            restore_vault_directories(moved);
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
        try {
            impl_->mirror->rebuild(session_mirror_root(candidate), *impl_->sessions);
        } catch (const std::exception& error) {
            log_warn(
                "Session mirror rebuild failed: " + std::string(error.what()));
            impl_->mirror->rebuild(std::nullopt, *impl_->sessions);
        }
    } else {
        impl_->publish_vault_names();
    }
    return candidate;
}

void ApplicationRuntime::delete_vault(std::string_view name) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
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

std::vector<std::string> ApplicationRuntime::list_r2_vaults() const {
    R2StorageKey storage;
    std::vector<std::string> local_database_names;
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        const std::optional<R2StorageKey> configured = impl_->api_keys->r2();
        if (!configured) {
            throw std::invalid_argument(
                "R2 storage credentials are not configured for the active vault");
        }
        storage = *configured;
        local_database_names.reserve(impl_->command.vaults.size());
        for (const VaultDefinition& vault : impl_->command.vaults) {
            local_database_names.push_back(utf8_path(vault.data.filename()));
        }
    }

    std::vector<std::string> names = list_r2_database_names(storage);
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

VaultDefinition ApplicationRuntime::download_r2_vault(std::string_view name) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    const std::optional<R2StorageKey> storage = impl_->api_keys->r2();
    if (!storage) {
        throw std::invalid_argument(
            "R2 storage credentials are not configured for the active vault");
    }
    const std::string database_name = std::string(name) + ".sqlite3";
    require_path_component(database_name, impl_->command.config_directory);

    VaultDefinition candidate{
        .data = normalized_vault_path(
            impl_->command.config_directory, path_from_utf8(database_name)),
        .source = next_vault_file(impl_->command.config_directory),
    };
    require_available_database_path(candidate.data);

    try {
        (void)download_new_database_from_r2(
            candidate.data, candidate.source, database_name, *storage);
        candidate = load_vault_definition_file(
            impl_->command.config_directory, candidate.source);
        assign_vault_paths(candidate, impl_->command);
        std::vector<VaultDefinition> updated = impl_->command.vaults;
        updated.push_back(candidate);
        validate_candidate_vaults(impl_->command.config_directory, updated);
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

void ApplicationRuntime::switch_vault(
    std::string_view name,
    std::string password) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    const VaultDefinition* const target =
        find_vault(impl_->command.vaults, name);
    if (target == nullptr) {
        throw UnknownVaultError(
            "Unknown vault '" + std::string(name) + "'");
    }
    if (same_vault_name(impl_->current_vault_.get().name, target->name)) {
        return;
    }
    if (target->password_protected && password.empty()) {
        throw VaultPasswordError("Password required to open this vault");
    }
    if (impl_->unusable) {
        throw WorkspaceRestartRequiredError(
            "The workspace database could not be reopened after an earlier "
            "maintenance operation. Restart is required");
    }
    if (!impl_->started || impl_->stopped) {
        throw std::runtime_error("CHA runtime is not running");
    }

    const VaultDefinition selected = *target;
    SessionLease target_lease = SessionLease::acquire(
        selected.data,
        "Database already in use: '" + utf8_path(selected.data) + "'");
    if (selected.password_protected) {
        require_openable_protected_database(selected.data, password);
    }
    require_switchable_database(selected.data, password);

    GlobalMaintenanceResult reserved =
        impl_->live_sessions->reserve_global_maintenance(
            impl_->settings.shutdown_grace);
    if (std::holds_alternative<MaintenanceFailure>(reserved)) {
        throw std::runtime_error(
            "Could not pause active sessions for database maintenance");
    }
    auto global = std::move(
        std::get<LiveSessionGlobalMaintenance>(reserved));
    Impl::AudioMaintenance audio(*impl_);
    {
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
    }
    try {
        impl_->api_keys->migrate_vault();
    } catch (const std::exception& error) {
        log_warn(
            "Legacy API-key migration failed after switching vault: "
            + std::string(error.what()));
    }

    try {
        impl_->mirror->rebuild(session_mirror_root(selected), *impl_->sessions);
    } catch (const std::exception& error) {
        log_warn(
            "Session mirror rebuild failed: " + std::string(error.what()));
        impl_->mirror->rebuild(std::nullopt, *impl_->sessions);
    }
    try {
        rewrite_toml_file(
            impl_->command.config_directory / "app.toml",
            [&](toml::table& table) {
                table.insert_or_assign("vault", selected.name);
            });
    } catch (const std::exception& error) {
        log_warn(
            "Failed to persist vault selection: " + std::string(error.what()));
    }
}

void ApplicationRuntime::merge_vault(
    std::string_view source_name,
    std::string password) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    const VaultDefinition* const source =
        find_vault(impl_->command.vaults, source_name);
    if (source == nullptr) {
        throw UnknownVaultError(
            "Unknown vault '" + std::string(source_name) + "'");
    }
    if (same_vault_name(impl_->current_vault_.get().name, source->name)) {
        throw std::invalid_argument("Cannot merge a vault into itself");
    }
    if (impl_->unusable) {
        throw WorkspaceRestartRequiredError(
            "The workspace database could not be reopened after an earlier "
            "maintenance operation. Restart is required");
    }
    if (!impl_->started || impl_->stopped) {
        throw std::runtime_error("CHA runtime is not running");
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
        impl_->stop_http();
        throw;
    }

    try {
        impl_->sessions->synchronize_forums();
    } catch (const std::exception& error) {
        impl_->unusable = true;
        impl_->stop_http();
        throw WorkspaceRestartRequiredError(
            std::string(
                "Configuration was committed but forums could not be "
                "synchronized: ")
            + error.what() + ". Restart is required");
    }

    for (const LiveSessionHandle& live :
         impl_->live_sessions->active_sessions()) {
        live->request_shutdown(ShutdownReason::reloading);
    }

    try {
        impl_->mirror->rebuild(
            session_mirror_root(impl_->current_vault_.get()), *impl_->sessions);
    } catch (const std::exception& error) {
        log_warn(
            "Session mirror rebuild failed: " + std::string(error.what()));
        impl_->mirror->rebuild(std::nullopt, *impl_->sessions);
    }
}

int ApplicationRuntime::start(int port_override) {
    const std::lock_guard operation(impl_->lifecycle_mutex);
    if (impl_->started) throw std::logic_error("CHA runtime is already started");

    auto server = std::make_unique<httplib::Server>();
    const auto http_settings = configure_http_server(
        *server, impl_->settings, !impl_->access_token.empty());
    if (!impl_->access_token.empty()) {
        const std::string token = impl_->access_token;
        server->set_pre_routing_handler(
            [token](const httplib::Request& request, httplib::Response& response) {
                if (cookie_matches(request, token)) {
                    return httplib::Server::HandlerResponse::Unhandled;
                }
                response.status = 404;
                response.set_content("Not found", "text/plain; charset=utf-8");
                response.set_header("Cache-Control", "no-store");
                return httplib::Server::HandlerResponse::Handled;
            });
    }

    const bool native_runtime = !impl_->access_token.empty();
    const AssetHandler assets(
        impl_->command.root / "web",
        [native_runtime]() {
            std::vector<std::string> result;
            if (!native_runtime) return result;
            const auto workspace = getws();
            if (workspace->voice_input()) {
                result.push_back(workspace->voice_input()->url);
            }
            return result;
        });
    assets.install(*server);
    const InitialSelection initial{
        {std::string(entrance_id), std::string(welcome_id)}};
    std::vector<std::string> vault_names;
    vault_names.reserve(impl_->command.vaults.size());
    for (const VaultDefinition& vault : impl_->command.vaults) {
        vault_names.push_back(vault.name);
    }
    LobbyRoutes(
        impl_->sessions,
        initial,
        *impl_->live_sessions,
        impl_->settings,
        *impl_->store,
        impl_->current_vault_,
        std::move(vault_names),
        impl_->mirror, [this](const FullSessionId& session) {
            // Wait through normal runtime operations, but stop waiting when
            // shutdown starts: it holds this lock while joining HTTP threads.
            std::unique_lock lifecycle(impl_->lifecycle_mutex, std::defer_lock);
            while (!impl_->stopping && !lifecycle.try_lock_for(std::chrono::milliseconds(10))) {}
            if (impl_->stopping || impl_->stopped || impl_->unusable)
                throw AudioDownloadError(503, "speech_busy", "Audio downloads are temporarily unavailable.");
            impl_->audio_downloads->clear(session);
        }).install(*server);
    install_vault_routes(*server, this, impl_->settings);
    OpenAiAuthRoutes(*impl_->openai_auth, impl_->settings).install(*server);
    SettingsRoutes(
        *impl_->live_sessions,
        impl_->settings,
        *impl_->store,
        *impl_->api_keys,
        *impl_->openai_auth,
        !impl_->access_token.empty(), impl_->fish_audio).install(*server);
    install_audio_download_routes(*server, *impl_->audio_downloads, impl_->settings);
    SessionRoutes(
        *impl_->live_sessions, impl_->settings, assets).install(*server);
    log_startup(http_settings);

    const int requested_port = port_override < 0
        ? impl_->command.port : port_override;
    int port = requested_port;
    if (requested_port == 0) {
        port = server->bind_to_any_port(impl_->command.host);
    } else if (!server->bind_to_port(impl_->command.host, requested_port)) {
        port = -1;
    }
    if (port < 1) {
        throw std::runtime_error(
            "Could not listen on " + impl_->command.host + ':'
            + std::to_string(requested_port));
    }

    impl_->server = std::move(server);
    impl_->listener = std::thread(
        [server = impl_->server.get()] { server->listen_after_bind(); });
    impl_->server->wait_until_ready();
    if (!impl_->server->is_running()) {
        if (impl_->listener.joinable()) impl_->listener.join();
        impl_->server.reset();
        throw std::runtime_error("CHA HTTP server could not start");
    }
    impl_->started = true;
    log_info(
        "web server event=bound address=" + impl_->command.host + ':'
        + std::to_string(port));
    return port;
}

void ApplicationRuntime::wait_for_shutdown_signal() {
    ProcessShutdownSignal signals;
    while (!signals.requested() && impl_->server->is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    shutdown();
}

void ApplicationRuntime::shutdown() {
    impl_->stopping = true;
    const std::lock_guard operation(impl_->lifecycle_mutex);
    if (!impl_->started || impl_->stopped) return;
    ServerShutdownCoordinator coordinator(
        *impl_->live_sessions,
        *impl_->server,
        [this] { impl_->stop_http(); },
        [this](auto deadline) { return impl_->audio_downloads->join_until(deadline); });
    coordinator.shutdown_now(impl_->listener, impl_->settings.shutdown_grace);
    impl_->providers.shutdown();
    impl_->stopped = true;
}

R2DatabaseTransfer ApplicationRuntime::upload_database() {
    return impl_->maintain_database([this] {
        const std::optional<R2StorageKey> storage = impl_->api_keys->r2();
        if (!storage) throw std::runtime_error("The active vault has no R2 key");
        const VaultDefinition vault = impl_->current_vault_.get();
        return upload_database_to_r2(
            vault.data,
            vault.source,
            *storage,
            R2DatabaseLease::already_held,
            impl_->active_password);
    }, false);
}

R2DatabaseTransfer ApplicationRuntime::download_database() {
    std::optional<VaultDefinition> downloaded_vault;
    const R2DatabaseTransfer result = impl_->maintain_database([this, &downloaded_vault] {
        const std::optional<R2StorageKey> storage = impl_->api_keys->r2();
        if (!storage) throw std::runtime_error("The active vault has no R2 key");
        const VaultDefinition vault = impl_->current_vault_.get();
        const R2DatabaseTransfer transferred = download_database_from_r2(
            vault.data,
            vault.source,
            *storage,
            R2DatabaseLease::already_held,
            impl_->active_password);
        downloaded_vault = load_vault_definition_file(
            impl_->command.config_directory, vault.source);
        assign_vault_paths(*downloaded_vault, impl_->command);
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
    try {
        impl_->mirror->rebuild(
            session_mirror_root(*downloaded_vault), *impl_->sessions);
    } catch (const std::exception& error) {
        log_warn(
            "Session mirror rebuild failed: " + std::string(error.what()));
        impl_->mirror->rebuild(std::nullopt, *impl_->sessions);
    }
    return result;
}

WorkspaceConfigTransfer ApplicationRuntime::import_configuration() {
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

WorkspaceConfigTransfer ApplicationRuntime::export_configuration() {
    return impl_->maintain_database([this] {
        const VaultDefinition vault = impl_->current_vault_.get();
        if (!vault.modify) {
            throw std::runtime_error(
                "Application config requires 'modify' for Export");
        }
        clear_existing_export(*vault.modify);
        return export_workspace_configuration(
            vault.data,
            *vault.modify,
            WorkspaceConfigLease::already_held,
            impl_->active_password);
    }, false);
}

} // namespace cha::web
