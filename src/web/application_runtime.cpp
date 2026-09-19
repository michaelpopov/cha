#include "web/application_runtime.h"

#include "app/application.h"
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
}

WebSettings http_application_settings(const ApplicationCommand& command) {
    WebSettings settings;
    configure_test_idle_grace(settings, command);
    settings.browser_disconnect_lifetime = true;
    settings.monotonic_event_sequence = false;
    return settings;
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

} // namespace

struct ApplicationRuntime::Impl {
    explicit Impl(
        const ApplicationCommand& selected_command,
        std::string selected_access_token,
        std::string selected_vault_password)
        : application(cha::app::Application::open(
              selected_command,
              std::move(selected_vault_password),
              http_application_settings(selected_command))),
          command(application->command()),
          access_token(std::move(selected_access_token)),
          active_password(application->mutable_active_password()),
          settings(application->settings()),
          current_vault_(application->current_vault()),
          store(&application->store()),
          sessions(application->sessions()),
          mirror(application->mirror()),
          api_keys(&application->api_keys()),
          openai_auth(&application->openai_auth()),
          providers(application->providers()),
          live_sessions(&application->live_sessions()),
          lifecycle_mutex(application->lifecycle_mutex()),
          unusable(application->mutable_unusable()) {
        audio_downloads = std::make_unique<AudioDownloadManager>(
            *sessions, current_vault_, true);
        application->set_resource_hooks({
            .pause = [this](bool cancel) { audio_downloads->pause(cancel); },
            .resume = [this] {
                if (!unusable) audio_downloads->resume();
            },
        });
        application->set_context_changed(
            [this](std::uint64_t, cha::app::ApplicationState state) {
                if (state == cha::app::ApplicationState::unavailable) {
                    stop_http();
                }
            });
    }

    ~Impl() {
        application->set_resource_hooks({});
        application->set_context_changed({});
    }

    std::unique_ptr<cha::app::Application> application;
    ApplicationCommand& command;
    std::string access_token;
    std::string& active_password;
    const WebSettings& settings;
    CurrentVault& current_vault_;
    WorkspaceConfigStore* store;
    std::shared_ptr<SessionRepository> sessions;
    std::shared_ptr<SessionMirror> mirror;
    ApiKeyStore* api_keys;
    OpenAiOAuth* openai_auth;
    Providers& providers;
    LiveSessionManager* live_sessions;
    FishAudioProxy fish_audio;
    std::unique_ptr<AudioDownloadManager> audio_downloads;
    std::unique_ptr<httplib::Server> server;
    std::thread listener;
    std::timed_mutex& lifecycle_mutex;
    std::atomic_bool stopping{};
    bool started{};
    bool stopped{};
    // Set when the workspace database could not be reopened. The HTTP server is
    // stopped at the same time because the runtime can no longer serve safely.
    bool& unusable;
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
    if (impl_ && !impl_->stopped) shutdown();
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
    return impl_->application->vault_snapshot();
}

std::string ApplicationRuntime::api_key_value(std::string_view id) const {
    return impl_->api_keys->value(id);
}

bool ApplicationRuntime::has_r2_storage() const {
    return impl_->application->has_r2_storage();
}

VaultDefinition ApplicationRuntime::create_vault(VaultCreate create) {
    return impl_->application->create_vault(std::move(create));
}

VaultDefinition ApplicationRuntime::update_vault(
    std::string_view current_name,
    VaultUpdate update) {
    return impl_->application->update_vault(current_name, std::move(update));
}

void ApplicationRuntime::delete_vault(std::string_view name) {
    impl_->application->delete_vault(name);
}

std::vector<std::string> ApplicationRuntime::list_r2_vaults() const {
    return impl_->application->list_r2_vaults();
}

VaultDefinition ApplicationRuntime::download_r2_vault(std::string_view name) {
    return impl_->application->download_r2_vault(name);
}

void ApplicationRuntime::switch_vault(
    std::string_view name,
    std::string password) {
    (void)impl_->application->switch_vault(name, std::move(password));
}

void ApplicationRuntime::merge_vault(
    std::string_view source_name,
    std::string password) {
    (void)impl_->application->merge_vault(source_name, std::move(password));
}

int ApplicationRuntime::start(int port_override) {
    const std::lock_guard operation(impl_->lifecycle_mutex);
    if (impl_->started) throw std::logic_error("CHA runtime is already started");

    auto server = std::make_unique<httplib::Server>();
    const auto http_settings = configure_http_server(
        *server, impl_->settings, true);
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
        []() {
            std::vector<std::string> result;
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
        true, impl_->fish_audio).install(*server);
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
    impl_->application->request_shutdown();
    {
        const std::lock_guard operation(impl_->lifecycle_mutex);
        if (impl_->stopped) return;
    }
    if (impl_->started) {
        ServerShutdownCoordinator coordinator(
            *impl_->live_sessions,
            *impl_->server,
            [this] { impl_->stop_http(); },
            [this](auto deadline) {
                return impl_->audio_downloads->join_until(deadline);
            });
        coordinator.shutdown_now(impl_->listener, impl_->settings.shutdown_grace);
    }
    (void)impl_->application->join_shutdown(impl_->settings.shutdown_grace);
    const std::lock_guard operation(impl_->lifecycle_mutex);
    impl_->stopped = true;
}

R2DatabaseTransfer ApplicationRuntime::upload_database() {
    return impl_->application->upload_database();
}

R2DatabaseTransfer ApplicationRuntime::download_database() {
    return impl_->application->download_database();
}

WorkspaceConfigTransfer ApplicationRuntime::import_configuration() {
    return impl_->application->import_configuration();
}

WorkspaceConfigTransfer ApplicationRuntime::export_configuration() {
    return impl_->application->export_configuration();
}

} // namespace cha::web
