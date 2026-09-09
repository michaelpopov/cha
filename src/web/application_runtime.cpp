#include "web/application_runtime.h"

#include "providers/openai_oauth.h"
#include "providers/api_key_store.h"
#include "providers/provider_client.h"
#include "providers/providers.h"
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
#include "workspace/builtins.h"
#include "workspace/session_open.h"
#include "workspace/workspace.h"
#include "workspace/workspace_config_store.h"
#include "web/asset_handler.h"
#include "web/http_response.h"
#include "web/http_server.h"
#include "web/json.h"
#include "web/live_session_manager.h"
#include "web/lobby_routes.h"
#include "web/openai_auth_routes.h"
#include "web/protocol.h"
#include "web/route_support.h"
#include "web/server_shutdown.h"
#include "web/session_mirror.h"
#include "web/session_routes.h"
#include "web/settings_routes.h"
#include "web/web_settings.h"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>

#include <algorithm>
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

void require_switchable_database(const std::filesystem::path& database) {
    const WorkspaceDatabaseState state =
        inspect_workspace_session_database(database);
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

std::optional<std::filesystem::path> normalized_absolute_vault_path(
    const std::optional<std::filesystem::path>& value,
    std::string_view field) {
    if (!value) return std::nullopt;
    if (value->empty() || !value->is_absolute()) {
        throw std::invalid_argument(
            "The " + std::string(field) + " path must be absolute");
    }
    return std::filesystem::weakly_canonical(*value);
}

toml::table vault_definition_table(const VaultDefinition& vault) {
    toml::table table;
    table.insert("vault_name", vault.name);
    table.insert("data", utf8_path(vault.data));
    if (vault.mirror) table.insert("mirror", utf8_path(*vault.mirror));
    if (vault.modify) table.insert("modify", utf8_path(*vault.modify));
    return table;
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
    if (vault.mirror && !std::filesystem::is_directory(*vault.mirror)) {
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

const std::string& required_json_string(
    const nlohmann::json& json,
    std::string_view key) {
    const std::string name(key);
    if (!json.is_object() || !json.contains(name) || !json.at(name).is_string()) {
        throw std::invalid_argument("Invalid vault settings");
    }
    return json.at(name).get_ref<const std::string&>();
}

std::optional<std::filesystem::path> nullable_json_path(
    const nlohmann::json& json,
    std::string_view key) {
    const std::string name(key);
    if (!json.is_object() || !json.contains(name)) {
        throw std::invalid_argument("Invalid vault settings");
    }
    if (json.at(name).is_null()) return std::nullopt;
    if (!json.at(name).is_string()) {
        throw std::invalid_argument("Invalid vault settings");
    }
    const std::string value = json.at(name).get<std::string>();
    if (value.empty()) throw std::invalid_argument("Invalid vault settings");
    return path_from_utf8(value);
}

std::optional<std::string> nullable_json_string(
    const nlohmann::json& json,
    std::string_view key) {
    const std::string name(key);
    if (!json.is_object() || !json.contains(name)) {
        throw std::invalid_argument("Invalid vault settings");
    }
    if (json.at(name).is_null()) return std::nullopt;
    if (!json.at(name).is_string()) {
        throw std::invalid_argument("Invalid vault settings");
    }
    const std::string value = json.at(name).get<std::string>();
    if (value.empty()) throw std::invalid_argument("Invalid vault settings");
    return value;
}

nlohmann::json vault_json(
    const VaultDefinition& vault,
    std::string_view active_name,
    std::size_t vault_count) {
    return {
        {"display_name", vault.name},
        {"data_path", utf8_path(vault.data)},
        {"mirror_path", vault.mirror
            ? nlohmann::json(utf8_path(*vault.mirror)) : nlohmann::json(nullptr)},
        {"modify_path", vault.modify
            ? nlohmann::json(utf8_path(*vault.modify)) : nlohmann::json(nullptr)},
        {"active", same_vault_name(vault.name, active_name)},
        {"can_delete", vault_count > 1
            && !same_vault_name(vault.name, active_name)},
    };
}

} // namespace

struct ApplicationRuntime::Impl {
    explicit Impl(
        const ApplicationCommand& selected_command,
        std::string selected_access_token)
        : command(selected_command),
          access_token(std::move(selected_access_token)),
          settings(),
          current_vault_(selected_command.vault),
          store(WorkspaceConfigStore::open(command.vault.data)),
          api_keys(std::make_unique<ApiKeyStore>(
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
            seed);
        mirror = std::make_shared<SessionMirror>();
        if (command.vault.mirror) {
            try {
                mirror->rebuild(*command.vault.mirror, *sessions);
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

    template<typename Operation>
    auto maintain_database(Operation operation) {
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
            if (server) server->stop();
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

    ApplicationCommand command;
    std::string access_token;
    WebSettings settings;
    CurrentVault current_vault_;
    std::unique_ptr<WorkspaceConfigStore> store;
    std::shared_ptr<SessionRepository> sessions;
    std::shared_ptr<SessionMirror> mirror;
    std::unique_ptr<ApiKeyStore> api_keys;
    std::unique_ptr<OpenAiOAuth> openai_auth;
    Providers providers;
    std::unique_ptr<LiveSessionManager> live_sessions;
    std::unique_ptr<httplib::Server> server;
    std::thread listener;
    mutable std::mutex lifecycle_mutex;
    bool started{};
    bool stopped{};
    // Set when the workspace database could not be reopened. The HTTP server is
    // stopped at the same time because the runtime can no longer serve safely.
    bool unusable{};
};

ApplicationRuntime::ApplicationRuntime(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

ApplicationRuntime::~ApplicationRuntime() {
    if (impl_ && impl_->started && !impl_->stopped) shutdown();
}

std::unique_ptr<ApplicationRuntime> ApplicationRuntime::open(
    const ApplicationCommand& command,
    std::string access_token) {
    load_dotenv(command.config_directory / ".env");
    return std::unique_ptr<ApplicationRuntime>(new ApplicationRuntime(
        std::make_unique<Impl>(command, std::move(access_token))));
}

VaultDefinition ApplicationRuntime::current_vault() const {
    return impl_->current_vault_.get();
}

VaultRegistrySnapshot ApplicationRuntime::vault_snapshot() const {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    return {impl_->command.vaults, impl_->current_vault_.get()};
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
        .data = normalized_vault_path(
            impl_->command.config_directory, create.data),
        .mirror = normalized_absolute_vault_path(create.mirror, "mirror"),
        .modify = normalized_absolute_vault_path(create.modify, "modify"),
        .source = next_vault_file(impl_->command.config_directory),
    };
    if (!std::filesystem::is_directory(candidate.data.parent_path())) {
        throw std::invalid_argument("The database parent directory does not exist");
    }
    std::error_code status_error;
    const std::filesystem::file_status data_status =
        std::filesystem::symlink_status(candidate.data, status_error);
    if (status_error && status_error != std::errc::no_such_file_or_directory) {
        throw std::runtime_error("The database path could not be inspected");
    }
    if (data_status.type() != std::filesystem::file_type::not_found) {
        throw std::invalid_argument("The database path already exists");
    }
    validate_vault_paths(candidate);

    std::vector<VaultDefinition> updated = impl_->command.vaults;
    updated.push_back(candidate);
    validate_candidate_vaults(impl_->command.config_directory, updated);

    write_toml_file(candidate.source, vault_definition_table(candidate));
    try {
        if (copied != nullptr) {
            copy_workspace_session_database(copied->data, candidate.data);
        } else {
            create_workspace_session_database_from_configuration(
                impl_->current_vault_.get().data, candidate.data);
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
    candidate.mirror = normalized_absolute_vault_path(update.mirror, "mirror");
    candidate.modify = normalized_absolute_vault_path(update.modify, "modify");
    validate_vault_paths(candidate);

    std::vector<VaultDefinition> updated = impl_->command.vaults;
    updated[static_cast<std::size_t>(found - impl_->command.vaults.begin())] =
        candidate;
    validate_candidate_vaults(impl_->command.config_directory, updated);

    const bool active = same_vault_name(
        impl_->current_vault_.get().name, previous.name);
    write_toml_file(candidate.source, vault_definition_table(candidate));
    try {
        if (active) {
            rewrite_toml_file(
                impl_->command.config_directory / "app.toml",
                [&](toml::table& table) {
                    table.insert_or_assign("vault", candidate.name);
                });
        }
    } catch (...) {
        write_toml_file(previous.source, vault_definition_table(previous));
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
            impl_->mirror->rebuild(candidate.mirror, *impl_->sessions);
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

void ApplicationRuntime::switch_vault(std::string_view name) {
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
    require_switchable_database(selected.data);

    GlobalMaintenanceResult reserved =
        impl_->live_sessions->reserve_global_maintenance(
            impl_->settings.shutdown_grace);
    if (std::holds_alternative<MaintenanceFailure>(reserved)) {
        throw std::runtime_error(
            "Could not pause active sessions for database maintenance");
    }
    auto global = std::move(
        std::get<LiveSessionGlobalMaintenance>(reserved));
    {
        auto database = impl_->store->reserve_maintenance();
        SessionRepository::MaintenanceGuard repository =
            impl_->sessions->reserve_maintenance();
        repository.checkpoint();
        database.close();
        database.retarget(selected.data, std::move(target_lease));
        repository.retarget(selected.data);
        impl_->reopen(database, repository);
        impl_->current_vault_.set(selected);
    }

    try {
        impl_->mirror->rebuild(selected.mirror, *impl_->sessions);
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

int ApplicationRuntime::start(int port_override) {
    const std::lock_guard operation(impl_->lifecycle_mutex);
    if (impl_->started) throw std::logic_error("CHA runtime is already started");

    auto server = std::make_unique<httplib::Server>();
    configure_http_server(*server, impl_->settings);
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

    const AssetHandler assets(
        impl_->command.root / "web",
        !impl_->access_token.empty() && impl_->command.voice_input
            ? std::optional<std::string>(impl_->command.voice_input->url)
            : std::nullopt);
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
        impl_->mirror).install(*server);
    ApplicationRuntime* const runtime = this;
    const WebSettings settings = impl_->settings;
    server->Get(
        "/api/v1/vaults",
        [runtime](const httplib::Request&, httplib::Response& response) {
            const VaultRegistrySnapshot snapshot = runtime->vault_snapshot();
            nlohmann::json result = nlohmann::json::array();
            for (const VaultDefinition& vault : snapshot.vaults) {
                result.push_back(vault_json(
                    vault, snapshot.active.name, snapshot.vaults.size()));
            }
            set_json_response(response, 200, result);
        });
    server->Post(
        "/api/v1/vaults",
        [runtime, settings](
            const httplib::Request& request, httplib::Response& response) {
            if (!validate_json_mutation(request, response)) return;
            VaultCreate create;
            if (!parse_route_json_body(
                    request, response, settings.request_body_limit,
                    [&create](const nlohmann::json& json) {
                        if (!json.is_object() || json.size() != 5) {
                            throw std::invalid_argument("Invalid vault settings");
                        }
                        create.display_name =
                            required_json_string(json, "display_name");
                        create.data = path_from_utf8(
                            required_json_string(json, "data_path"));
                        create.mirror = nullable_json_path(json, "mirror_path");
                        create.modify = nullable_json_path(json, "modify_path");
                        create.copy_from =
                            nullable_json_string(json, "copy_from");
                    })) return;
            try {
                const VaultDefinition created =
                    runtime->create_vault(std::move(create));
                const VaultRegistrySnapshot snapshot =
                    runtime->vault_snapshot();
                set_json_response(
                    response, 201,
                    vault_json(
                        created, snapshot.active.name, snapshot.vaults.size()));
            } catch (const std::invalid_argument& error) {
                set_error_response(
                    response, 400, {ErrorCode::bad_request, error.what()});
            } catch (const std::exception& error) {
                set_error_response(
                    response, 500, {ErrorCode::internal_error, error.what()});
            }
        });
    server->Patch(
        "/api/v1/vaults",
        [runtime, settings](
            const httplib::Request& request, httplib::Response& response) {
            if (!validate_json_mutation(request, response)) return;
            std::string name;
            VaultUpdate update;
            if (!parse_route_json_body(
                    request, response, settings.request_body_limit,
                    [&name, &update](const nlohmann::json& json) {
                        if (!json.is_object() || json.size() != 4) {
                            throw std::invalid_argument("Invalid vault settings");
                        }
                        name = required_json_string(json, "vault_name");
                        update.display_name =
                            required_json_string(json, "display_name");
                        update.mirror = nullable_json_path(json, "mirror_path");
                        update.modify = nullable_json_path(json, "modify_path");
                    })) return;
            try {
                const VaultDefinition updated =
                    runtime->update_vault(name, std::move(update));
                const VaultRegistrySnapshot snapshot =
                    runtime->vault_snapshot();
                set_json_response(
                    response, 200,
                    vault_json(
                        updated, snapshot.active.name, snapshot.vaults.size()));
            } catch (const std::out_of_range&) {
                set_route_not_found(response, "That vault was not found.");
            } catch (const std::invalid_argument& error) {
                set_error_response(
                    response, 400, {ErrorCode::bad_request, error.what()});
            } catch (const std::exception& error) {
                set_error_response(
                    response, 500, {ErrorCode::internal_error, error.what()});
            }
        });
    server->Delete(
        "/api/v1/vaults",
        [runtime, settings](
            const httplib::Request& request, httplib::Response& response) {
            if (!validate_json_mutation(request, response)) return;
            std::string name;
            if (!parse_route_json_body(
                    request, response, settings.request_body_limit,
                    [&name](const nlohmann::json& json) {
                        if (!json.is_object() || json.size() != 1) {
                            throw std::invalid_argument("Invalid vault settings");
                        }
                        name = required_json_string(json, "vault_name");
                    })) return;
            try {
                runtime->delete_vault(name);
                response.status = 204;
                response.set_header("Cache-Control", "no-store");
            } catch (const std::out_of_range&) {
                set_route_not_found(response, "That vault was not found.");
            } catch (const std::invalid_argument& error) {
                set_error_response(
                    response, 409, {ErrorCode::bad_request, error.what()});
            } catch (const std::exception& error) {
                set_error_response(
                    response, 500, {ErrorCode::internal_error, error.what()});
            }
        });
    server->Post(
        "/api/v1/vault/switch",
        [runtime, settings](
            const httplib::Request& request, httplib::Response& response) {
            if (!validate_json_mutation(request, response)) return;
            std::string vault_name;
            if (!parse_route_json_body(
                    request,
                    response,
                    settings.request_body_limit,
                    [&vault_name](const nlohmann::json& json) {
                        vault_name = parse_vault_switch_name(json);
                    })) {
                return;
            }
            try {
                runtime->switch_vault(vault_name);
            } catch (const UnknownVaultError& error) {
                return set_error_response(
                    response, 400, {ErrorCode::bad_request, error.what()});
            } catch (const std::exception& error) {
                return set_error_response(
                    response, 500, {ErrorCode::internal_error, error.what()});
            }
            response.status = 204;
            response.set_header("Cache-Control", "no-store");
        });
    OpenAiAuthRoutes(*impl_->openai_auth, impl_->settings).install(*server);
    SettingsRoutes(
        *impl_->live_sessions,
        impl_->settings,
        *impl_->store,
        *impl_->api_keys,
        *impl_->openai_auth,
        impl_->command.voice_input
            ? impl_->command.voice_input->api_key_id
            : std::string{}).install(*server);
    SessionRoutes(
        *impl_->live_sessions, impl_->settings, assets).install(*server);
    log_startup(impl_->settings);

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
    const std::lock_guard operation(impl_->lifecycle_mutex);
    if (!impl_->started || impl_->stopped) return;
    ServerShutdownCoordinator coordinator(
        *impl_->live_sessions, *impl_->server);
    coordinator.shutdown_now(impl_->listener, impl_->settings.shutdown_grace);
    impl_->providers.shutdown();
    impl_->stopped = true;
}

R2DatabaseTransfer ApplicationRuntime::upload_database() {
    return impl_->maintain_database([this] {
        const VaultDefinition vault = impl_->current_vault_.get();
        return upload_database_to_r2(
            vault.data, R2DatabaseLease::already_held);
    });
}

R2DatabaseTransfer ApplicationRuntime::download_database() {
    return impl_->maintain_database([this] {
        const VaultDefinition vault = impl_->current_vault_.get();
        return download_database_from_r2(
            vault.data, R2DatabaseLease::already_held);
    });
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
            WorkspaceConfigLease::already_held);
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
            WorkspaceConfigLease::already_held);
    });
}

} // namespace cha::web
