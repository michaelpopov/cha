#include "web/application_runtime.h"

#include "providers/providers.h"
#include "session/session_repository.h"
#include "util/logging.h"
#include "workspace/builtins.h"
#include "workspace/session_open.h"
#include "workspace/workspace.h"
#include "workspace/workspace_config_store.h"
#include "web/asset_handler.h"
#include "web/http_server.h"
#include "web/live_session_manager.h"
#include "web/lobby_routes.h"
#include "web/server_shutdown.h"
#include "web/session_mirror.h"
#include "web/session_routes.h"
#include "web/web_settings.h"

#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>

namespace cha::web {
namespace {

void configure_test_idle_grace(
    WebSettings& settings,
    const ApplicationCommand& command) {
    if (!command.test_idle_grace_ms) return;
    settings.idle_grace =
        std::chrono::milliseconds(*command.test_idle_grace_ms);
    settings.orphan_limit = std::max(
        settings.orphan_limit, settings.idle_grace);
    const auto max_interval = std::max(
        std::chrono::milliseconds{1}, settings.idle_grace / 2);
    settings.sse_heartbeat_interval = std::min(
        settings.sse_heartbeat_interval, max_interval);
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

} // namespace

struct ApplicationRuntime::Impl {
    explicit Impl(
        const ApplicationCommand& selected_command,
        std::string selected_access_token)
        : command(selected_command),
          access_token(std::move(selected_access_token)),
          store(WorkspaceConfigStore::open(command.database)) {
        configure_test_idle_grace(settings, command);
        const auto seed = TemporarySessionSeed{
            {std::string(entrance_id), std::string(welcome_id)},
            std::string(welcome_name)};
        sessions = std::make_shared<const SessionRepository>(
            store->database_path(),
            store->workspace_path(),
            store->welcome_path(),
            seed);
        if (command.mirror) {
            mirror = std::make_shared<SessionMirror>(
                *command.mirror, *sessions);
        }

        auto opener = [this](
                          const FullSessionId& identity,
                          std::shared_ptr<WakeNotifier> notifier) {
            OpenedSession opened = open_session(
                *sessions, identity, providers, std::move(notifier), *store);
            if (mirror) {
                const auto selected_mirror = mirror;
                opened.mirror = [selected_mirror, identity](
                                    std::string_view label,
                                    std::span<const TranscriptEntry> entries) {
                    selected_mirror->update(identity, label, entries);
                };
            }
            return opened;
        };
        live_sessions = std::make_unique<LiveSessionManager>(settings, opener);
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
        const SessionRepository::MaintenanceGuard repository =
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
    std::unique_ptr<WorkspaceConfigStore> store;
    std::shared_ptr<const SessionRepository> sessions;
    std::shared_ptr<SessionMirror> mirror;
    Providers providers;
    std::unique_ptr<LiveSessionManager> live_sessions;
    std::unique_ptr<httplib::Server> server;
    std::thread listener;
    std::mutex lifecycle_mutex;
    bool started{};
    bool stopped{};
    // Set when the workspace database could not be reopened after a transfer.
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
    return std::unique_ptr<ApplicationRuntime>(new ApplicationRuntime(
        std::make_unique<Impl>(command, std::move(access_token))));
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

    const AssetHandler assets(impl_->command.root / "web");
    assets.install(*server);
    const InitialSelection initial{
        {std::string(entrance_id), std::string(welcome_id)}};
    LobbyRoutes(
        impl_->sessions,
        initial,
        *impl_->live_sessions,
        impl_->settings,
        *impl_->store,
        impl_->mirror).install(*server);
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
        return upload_database_to_r2(
            impl_->command.database, R2DatabaseLease::already_held);
    });
}

R2DatabaseTransfer ApplicationRuntime::download_database() {
    return impl_->maintain_database([this] {
        return download_database_from_r2(
            impl_->command.database, R2DatabaseLease::already_held);
    });
}

WorkspaceConfigTransfer ApplicationRuntime::import_configuration() {
    if (!impl_->command.modify) {
        throw std::runtime_error(
            "Application config requires 'modify' for Import");
    }
    return impl_->maintain_database([this] {
        return import_workspace_configuration(
            *impl_->command.modify,
            impl_->command.database,
            WorkspaceConfigLease::already_held);
    });
}

WorkspaceConfigTransfer ApplicationRuntime::export_configuration() {
    if (!impl_->command.modify) {
        throw std::runtime_error(
            "Application config requires 'modify' for Export");
    }
    return impl_->maintain_database([this] {
        if (std::filesystem::is_directory(*impl_->command.modify)) {
            std::filesystem::remove_all(*impl_->command.modify);
        }
        return export_workspace_configuration(
            impl_->command.database,
            *impl_->command.modify,
            WorkspaceConfigLease::already_held);
    });
}

} // namespace cha::web
