#include "workspace/builtins.h"
#include "workspace/session_open.h"
#include "workspace/workspace.h"
#include "providers/providers.h"
#include "session/session_repository.h"
#include "session/session_migration.h"
#include "web/application_config.h"
#include "web/asset_handler.h"
#include "web/http_server.h"
#include "web/lobby_routes.h"
#include "web/session_routes.h"
#include "web/live_session_manager.h"
#include "web/server_shutdown.h"
#include "web/web_settings.h"
#include "util/environment.h"
#include "util/logging.h"
#include "util/path_name.h"

#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

using namespace cha;
using namespace web;

static int prepare_and_run(int argc, const char* argv[]);
static int run_web_server(
    const std::string& host, int port,
    const std::filesystem::path& root,
    const std::filesystem::path& backup_dir,
    const std::shared_ptr<const SessionRepository>& sessions,
    LiveSessionManager& live_sessions,
    const WebSettings& settings);
static void log_web_server_startup(const WebSettings& settings);
static void configure_test_idle_grace(WebSettings& settings, const ApplicationConfig& app);
static int run_migration(const ApplicationConfig& app);

int main(int argc, const char* argv[]) {
    try {
        return prepare_and_run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "Failed: " << error.what() << '\n';
        return 1;
    }
}

int prepare_and_run(int argc, const char* argv[]) {
    const ApplicationConfig app = load_application_config(argc, argv);
    if (app.migration) return run_migration(app);
    load_dotenv(app.workspace / ".env");

    WebSettings settings;
    configure_test_idle_grace(settings, app);

    loadws(app.workspace);
    const std::shared_ptr<const Workspace> workspace = getws();
    initialize_diagnostic_logging(
        workspace->settings().log_file, workspace->settings().log_level);

    const auto seed = TemporarySessionSeed{{std::string(entrance_id), std::string(welcome_id)}, std::string(welcome_name)};
    const auto sessions =
        std::make_shared<const SessionRepository>(app.workspace, seed);
    Providers providers;

    auto opener = [sessions, &providers](
                      const FullSessionId& identity,
                      std::shared_ptr<WakeNotifier> notifier) {
        return open_session(*sessions, identity, providers, std::move(notifier));
    };
    LiveSessionManager live_sessions(settings, opener);

    const int result = run_web_server(
        app.host, app.port, app.root, app.backup_dir, sessions, live_sessions, settings);
    providers.shutdown();
    shutdown_diagnostic_logging();
    return result;
}

int run_migration(const ApplicationConfig& app) {
    std::cout << "Validating workspace and legacy session databases...\n";
    const Workspace workspace = Workspace::load(app.workspace);
    const SessionMigrationSummary summary = migrate_sessions(workspace);
    std::cout
        << "Migration completed.\n\n"
        << "Forums: " << summary.forums << '\n'
        << "Active sessions: " << summary.active_sessions << '\n'
        << "Archived sessions: " << summary.archived_sessions << '\n'
        << "Turns: " << summary.turns << '\n'
        << "Entries: " << summary.entries << '\n'
        << "Database: " << utf8_path(summary.database_path) << "\n\n"
        << "Legacy session files were not changed.\n"
        << "Do not run the old CHA application after this migration.\n";
    return 0;
}

int run_web_server(
    const std::string& host, int port,
    const std::filesystem::path& root,
    const std::filesystem::path& backup_dir,
    const std::shared_ptr<const SessionRepository>& sessions,
    LiveSessionManager& live_sessions,
    const WebSettings& settings) {

    httplib::Server server;
    configure_http_server(server, settings);

    const AssetHandler assets(root / "web");
    assets.install(server);

    const InitialSelection initial{{std::string(entrance_id), std::string(welcome_id)}};
    LobbyRoutes(sessions, initial, live_sessions, backup_dir, settings).install(server);
    SessionRoutes(live_sessions, settings, assets).install(server);

    log_web_server_startup(settings);

    if (!server.bind_to_port(host, port)) {
        const std::string message = "Could not listen on " + host + ':' + std::to_string(port);
        log_error(message);
        std::cerr << "Failed: " << message << '\n';
        return 1;
    }

    log_info("web server event=bound address=" + host + ':' + std::to_string(port));
    std::thread listener([&server] { server.listen_after_bind(); });
    server.wait_until_ready();

    std::cout << "CHA ready at " << host << ':' << port << '\n' << std::flush;

    ProcessShutdownSignal signals;
    ServerShutdownCoordinator shutdown(live_sessions, server);
    shutdown.wait_and_shutdown(signals, listener, settings.shutdown_grace);

    return 0;
}

void log_web_server_startup(const WebSettings& settings) {
    log_info(
        "web server event=startup session_limit="
        + std::to_string(settings.session_limit)
        + " http_thread_pool_size=" + std::to_string(settings.http_thread_pool_size)
        + " http_pending_request_limit=" + std::to_string(settings.http_pending_request_limit)
        + " command_queue_capacity=" + std::to_string(settings.command_queue_capacity)
        + " request_body_limit=" + std::to_string(settings.request_body_limit)
        + " prompt_limit=" + std::to_string(settings.prompt_limit)
        + " http_read_timeout_ms=" + std::to_string(settings.http_read_timeout.count())
        + " http_write_timeout_ms=" + std::to_string(settings.http_write_timeout.count())
        + " open_deadline_ms=" + std::to_string(settings.open_deadline.count())
        + " command_deadline_ms=" + std::to_string(settings.command_deadline.count()));
}

void configure_test_idle_grace(WebSettings& settings, const ApplicationConfig& app) {
    if (app.test_idle_grace_ms) {
        settings.idle_grace = std::chrono::milliseconds(*app.test_idle_grace_ms);
        settings.orphan_limit = std::max(settings.orphan_limit, settings.idle_grace);
        const auto max_interval = std::max(std::chrono::milliseconds{1}, settings.idle_grace / 2);
        settings.sse_heartbeat_interval = std::min(settings.sse_heartbeat_interval, max_interval);
    }
}
