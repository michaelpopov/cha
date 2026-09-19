#include "runtime_bridge.h"

#include "app/application.h"
#include "bridge/bridge_router.h"
#include "util/logging.h"
#include "util/path_name.h"
#include "web/application_config.h"
#include "web/application_runtime.h"
#include "workspace/workspace_config_store.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using cha::WorkspaceConfigTransfer;
using cha::app::Application;
using cha::bridge::BridgeRouter;
using cha::web::ApplicationCommand;
using cha::web::ApplicationRuntime;
using cha::web::ConfigurationTransport;
using cha::web::parse_application_command;

namespace {

#if defined(_WIN32)
constexpr const char* kPlatform = "windows";
#elif defined(__APPLE__)
constexpr const char* kPlatform = "macos";
#else
constexpr const char* kPlatform = "unknown";
#endif

} // namespace

struct ChaRuntime {
    std::unique_ptr<Application> native_application;
    std::unique_ptr<BridgeRouter> router;
    std::unique_ptr<ApplicationRuntime> http_application;
    int port{};
    bool logging{};
    bool native{};
    bool joined{};

    std::mutex callback_mutex;
    cha_runtime_delivery_fn delivery_callback{};
    void* delivery_context{};

    std::mutex connections_mutex;
    std::vector<std::string> connections;

    std::atomic<bool> pump_stop{false};
    std::thread pump;
};

namespace {

void clear_error(char** error) {
    if (error) *error = nullptr;
}

void set_string(char** destination, const char* message) noexcept {
    if (!destination) return;
    const std::size_t size = std::strlen(message) + 1;
    char* const copy = static_cast<char*>(std::malloc(size));
    if (!copy) return;
    std::memcpy(copy, message, size);
    *destination = copy;
}

void set_current_error(char** error) noexcept {
    try {
        throw;
    } catch (const std::exception& exception) {
        set_string(error, exception.what());
    } catch (...) {
        set_string(error, "CHA failed with an unknown error");
    }
}

ApplicationCommand runtime_command(
    const char* config_path,
    const char* resource_path,
    bool http) {
    const char* arguments[] = {
        "CHA", "--root", resource_path, "--config", config_path};
    ApplicationCommand command = parse_application_command(
        5,
        arguments,
        http ? ConfigurationTransport::http : ConfigurationTransport::native);
    if (http) {
        // CHA.app does not use [web]. The only client is the WebView in this
        // process, so the listener is always private loopback on a port the
        // operating system picks.
        command.host = "127.0.0.1";
        command.port = 0;
    }
    return command;
}

void stop_pump(ChaRuntime* runtime) {
    if (!runtime) return;
    runtime->pump_stop.store(true);
    if (runtime->router) runtime->router->shutdown();
}

void join_pump(ChaRuntime* runtime) {
    if (runtime && runtime->pump.joinable()) runtime->pump.join();
}

void run_pump(ChaRuntime* runtime) {
    using namespace std::chrono_literals;
    while (!runtime->pump_stop.load()) {
        if (!runtime->router) break;
        runtime->router->wait_for_work(50ms);
        if (runtime->pump_stop.load()) break;
        try {
            runtime->router->run_tasks();
            runtime->router->expire_timeouts();
            runtime->router->pump_output();
        } catch (...) {
            continue;
        }
        std::vector<std::string> ids;
        {
            std::lock_guard lock(runtime->connections_mutex);
            ids = runtime->connections;
        }
        for (const auto& id : ids) {
            if (runtime->pump_stop.load()) break;
            std::optional<nlohmann::json> batch;
            try {
                batch = runtime->router->take_delivery(id);
            } catch (...) {
                continue;
            }
            if (!batch) continue;
            const std::string json = batch->dump();
            std::lock_guard lock(runtime->callback_mutex);
            if (runtime->delivery_callback) {
                runtime->delivery_callback(
                    runtime->delivery_context, id.c_str(), json.c_str());
            }
        }
    }
}

int32_t transfer(
    ChaRuntime* runtime,
    uint64_t* byte_count,
    char** error,
    bool download) {
    clear_error(error);
    if (!runtime || !byte_count) {
        set_string(error, "That database operation is not available.");
        return 0;
    }
    try {
        const cha::web::R2DatabaseTransfer result = runtime->native_application
            ? (download
                ? runtime->native_application->download_database()
                : runtime->native_application->upload_database())
            : runtime->http_application
            ? (download
                ? runtime->http_application->download_database()
                : runtime->http_application->upload_database())
            : throw std::runtime_error("CHA runtime is not available");
        *byte_count = result.byte_count;
        return 1;
    } catch (const cha::WorkspaceRestartRequiredError& fatal) {
        set_string(error, fatal.what());
        return -1;
    } catch (...) {
        set_current_error(error);
        return 0;
    }
}

int32_t transfer_configuration(
    ChaRuntime* runtime,
    uint64_t* file_count,
    char** error,
    bool importing) {
    clear_error(error);
    if (!runtime || !file_count) {
        set_string(error, "That database operation is not available.");
        return 0;
    }
    try {
        const WorkspaceConfigTransfer result = runtime->native_application
            ? (importing
                ? runtime->native_application->import_configuration()
                : runtime->native_application->export_configuration())
            : runtime->http_application
            ? (importing
                ? runtime->http_application->import_configuration()
                : runtime->http_application->export_configuration())
            : throw std::runtime_error("CHA runtime is not available");
        *file_count = result.file_count;
        return 1;
    } catch (const cha::WorkspaceRestartRequiredError& fatal) {
        set_string(error, fatal.what());
        return -1;
    } catch (...) {
        set_current_error(error);
        return 0;
    }
}

} // namespace

ChaRuntime* cha_runtime_create(
    const char* config_path,
    const char* resource_path,
    const char* access_token,
    const char* vault_password,
    int32_t http_mode,
    int32_t* password_error,
    char** error) {
    clear_error(error);
    if (password_error) *password_error = 0;
    const bool http = http_mode != 0;
    if (!config_path || !resource_path || !vault_password
        || (http && (!access_token || *access_token == '\0'))) {
        set_string(error, "CHA runtime configuration is incomplete");
        return nullptr;
    }

    std::unique_ptr<ChaRuntime> runtime;
    try {
        ApplicationCommand command = runtime_command(
            config_path, resource_path, http);
        runtime = std::make_unique<ChaRuntime>();
        cha::initialize_diagnostic_logging(
            command.log_file, command.log_level);
        runtime->logging = true;
        if (http) {
            runtime->http_application = ApplicationRuntime::open(
                command, access_token, vault_password);
            runtime->port = runtime->http_application->start();
            runtime->native = false;
            return runtime.release();
        }
        runtime->native_application = Application::open(
            command, vault_password, cha::app::native_settings());
        BridgeRouter::Options options;
        options.platform = kPlatform;
        runtime->router = std::make_unique<BridgeRouter>(
            *runtime->native_application, std::move(options));
        runtime->native = true;
        runtime->port = 0;
        runtime->pump = std::thread(run_pump, runtime.get());
        return runtime.release();
    } catch (const cha::web::VaultPasswordError&) {
        if (password_error) *password_error = 1;
        if (runtime && runtime->logging) {
            cha::shutdown_diagnostic_logging();
        }
        set_current_error(error);
        return nullptr;
    } catch (...) {
        if (runtime) {
            stop_pump(runtime.get());
            join_pump(runtime.get());
        }
        if (runtime && runtime->logging) {
            cha::shutdown_diagnostic_logging();
        }
        set_current_error(error);
        return nullptr;
    }
}

int32_t cha_runtime_requires_password(
    const char* config_path,
    const char* resource_path,
    char** vault_name,
    char** error) {
    clear_error(error);
    if (vault_name) *vault_name = nullptr;
    if (!config_path || !resource_path) {
        set_string(error, "CHA runtime configuration is incomplete");
        return -1;
    }
    try {
        const ApplicationCommand command = runtime_command(
            config_path, resource_path, false);
        set_string(vault_name, command.vault.name.c_str());
        if (vault_name && !*vault_name) throw std::bad_alloc();
        return command.vault.password_protected ? 1 : 0;
    } catch (...) {
        set_current_error(error);
        return -1;
    }
}

void cha_runtime_destroy(ChaRuntime* runtime) {
    if (!runtime) return;
    {
        std::lock_guard lock(runtime->callback_mutex);
        runtime->delivery_callback = nullptr;
        runtime->delivery_context = nullptr;
    }
    stop_pump(runtime);
    join_pump(runtime);
    if (runtime->native_application && !runtime->joined) {
        try {
            runtime->native_application->request_shutdown();
            (void)runtime->native_application->join_shutdown(
                std::chrono::milliseconds{10000});
            runtime->joined = true;
        } catch (...) {
            (void)runtime->native_application.release();
            if (runtime->logging) cha::shutdown_diagnostic_logging();
            return;
        }
    }
    runtime->router.reset();
    runtime->native_application.reset();
    if (runtime->http_application) {
        try {
            runtime->http_application->shutdown();
            runtime->http_application.reset();
        } catch (...) {
            (void)runtime->http_application.release();
            if (runtime->logging) cha::shutdown_diagnostic_logging();
            return;
        }
    }
    if (runtime->logging) cha::shutdown_diagnostic_logging();
    delete runtime;
}

int32_t cha_runtime_port(const ChaRuntime* runtime) {
    return runtime ? runtime->port : 0;
}

int32_t cha_runtime_is_native(const ChaRuntime* runtime) {
    return runtime && runtime->native ? 1 : 0;
}

int32_t cha_runtime_can_modify(const ChaRuntime* runtime) {
    if (!runtime) return 0;
    try {
        if (runtime->native_application) {
            return runtime->native_application->capabilities().can_modify ? 1 : 0;
        }
        if (runtime->http_application) {
            return runtime->http_application->current_vault().modify ? 1 : 0;
        }
    } catch (...) {
    }
    return 0;
}

int32_t cha_runtime_can_transfer_r2(const ChaRuntime* runtime) {
    if (!runtime) return 0;
    try {
        if (runtime->native_application) {
            return runtime->native_application->capabilities().can_transfer_r2 ? 1 : 0;
        }
        if (runtime->http_application) {
            return runtime->http_application->has_r2_storage() ? 1 : 0;
        }
    } catch (...) {
    }
    return 0;
}

int32_t cha_runtime_upload(
    ChaRuntime* runtime,
    uint64_t* byte_count,
    char** error) {
    return transfer(runtime, byte_count, error, false);
}

int32_t cha_runtime_download(
    ChaRuntime* runtime,
    uint64_t* byte_count,
    char** error) {
    return transfer(runtime, byte_count, error, true);
}

int32_t cha_runtime_import_configuration(
    ChaRuntime* runtime,
    uint64_t* file_count,
    char** error) {
    return transfer_configuration(runtime, file_count, error, true);
}

int32_t cha_runtime_export_configuration(
    ChaRuntime* runtime,
    uint64_t* file_count,
    char** error) {
    return transfer_configuration(runtime, file_count, error, false);
}

int32_t cha_runtime_context_epoch(
    const ChaRuntime* runtime,
    uint64_t* epoch) {
    if (!runtime || !runtime->native_application || !epoch) return 0;
    *epoch = runtime->native_application->context_epoch();
    return 1;
}

int32_t cha_runtime_export_session(
    ChaRuntime* runtime,
    uint64_t context_epoch,
    const char* forum_id,
    const char* session_id,
    const char* destination_utf8,
    char** error) {
    clear_error(error);
    if (!runtime || !runtime->native_application
        || !forum_id || !session_id || !destination_utf8
        || destination_utf8[0] == '\0') {
        set_string(error, "That file operation is not available.");
        return 0;
    }
    try {
        const auto exported = runtime->native_application->export_session(
            forum_id, session_id, context_epoch);
        runtime->native_application->save_file(
            context_epoch,
            cha::path_from_utf8(destination_utf8),
            exported.markdown);
        return 1;
    } catch (const cha::app::ApplicationError& denied) {
        set_string(
            error,
            std::string(cha::bridge::public_error_message(denied.code)).c_str());
        return 0;
    } catch (const cha::WorkspaceRestartRequiredError& fatal) {
        set_string(error, fatal.what());
        return -1;
    } catch (...) {
        set_current_error(error);
        return -1;
    }
}

int32_t cha_runtime_save_file(
    ChaRuntime* runtime,
    uint64_t context_epoch,
    const char* destination_utf8,
    const char* data,
    uint64_t size,
    char** error) {
    clear_error(error);
    if (!runtime || !runtime->native_application
        || !destination_utf8 || destination_utf8[0] == '\0'
        || (!data && size != 0)) {
        set_string(error, "That file operation is not available.");
        return 0;
    }
    try {
        const std::string_view contents =
            size == 0
                ? std::string_view{}
                : std::string_view(data, static_cast<std::size_t>(size));
        runtime->native_application->save_file(
            context_epoch,
            cha::path_from_utf8(destination_utf8),
            contents);
        return 1;
    } catch (const cha::app::ApplicationError& denied) {
        set_string(
            error,
            std::string(cha::bridge::public_error_message(denied.code)).c_str());
        return 0;
    } catch (const cha::WorkspaceRestartRequiredError& fatal) {
        set_string(error, fatal.what());
        return -1;
    } catch (...) {
        set_current_error(error);
        return -1;
    }
}

void cha_runtime_set_delivery_callback(
    ChaRuntime* runtime,
    cha_runtime_delivery_fn callback,
    void* context) {
    if (!runtime) return;
    std::lock_guard lock(runtime->callback_mutex);
    runtime->delivery_callback = callback;
    runtime->delivery_context = context;
}

char* cha_runtime_open_connection(ChaRuntime* runtime, char** error) {
    clear_error(error);
    if (!runtime || !runtime->router) {
        set_string(error, "CHA runtime is not available");
        return nullptr;
    }
    try {
        std::string id = runtime->router->open_connection();
        {
            std::lock_guard lock(runtime->connections_mutex);
            runtime->connections.push_back(id);
        }
        char* copy = nullptr;
        set_string(&copy, id.c_str());
        if (!copy) throw std::bad_alloc();
        return copy;
    } catch (...) {
        set_current_error(error);
        return nullptr;
    }
}

void cha_runtime_close_connection(
    ChaRuntime* runtime,
    const char* connection_id) {
    if (!runtime || !runtime->router || !connection_id) return;
    try {
        runtime->router->close_connection(connection_id);
        std::lock_guard lock(runtime->connections_mutex);
        auto& ids = runtime->connections;
        ids.erase(
            std::remove(ids.begin(), ids.end(), std::string(connection_id)),
            ids.end());
    } catch (...) {
    }
}

void cha_runtime_handle_message(
    ChaRuntime* runtime,
    const char* connection_id,
    const char* json) {
    if (!runtime || !runtime->router || !connection_id || !json) return;
    try {
        const nlohmann::json parsed = nlohmann::json::parse(json, nullptr, false);
        if (parsed.is_discarded() || !parsed.is_object()) return;
        if (parsed.contains("method")) {
            runtime->router->handle_request(connection_id, json);
        } else {
            runtime->router->handle_ack(connection_id, json);
        }
    } catch (...) {
    }
}

void cha_runtime_request_shutdown(ChaRuntime* runtime) {
    if (!runtime) return;
    stop_pump(runtime);
    if (runtime->native_application) {
        runtime->native_application->request_shutdown();
    }
}

int32_t cha_runtime_join_shutdown(ChaRuntime* runtime, int32_t grace_ms) {
    if (!runtime) return 1;
    join_pump(runtime);
    if (runtime->native_application && !runtime->joined) {
        runtime->joined = true;
        const auto grace = std::chrono::milliseconds{
            grace_ms > 0 ? grace_ms : 10000};
        return runtime->native_application->join_shutdown(grace) ? 1 : 0;
    }
    if (runtime->http_application) {
        try {
            runtime->http_application->shutdown();
        } catch (...) {
            return 0;
        }
    }
    return 1;
}

void cha_string_free(char* value) {
    std::free(value);
}
