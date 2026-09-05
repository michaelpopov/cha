#include "runtime_bridge.h"

#include "util/logging.h"
#include "web/application_config.h"
#include "web/application_runtime.h"
#include "workspace/workspace_config_store.h"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <new>
#include <string>

using cha::WorkspaceConfigTransfer;
using cha::import_workspace_configuration;
using cha::web::ApplicationCommand;
using cha::web::ApplicationRuntime;
using cha::web::R2DatabaseTransfer;
using cha::web::parse_application_command;

struct ChaRuntime {
    std::unique_ptr<ApplicationRuntime> application;
    int port{};
    bool can_modify{};
    bool logging{};
};

namespace {

void clear_error(char** error) {
    if (error) *error = nullptr;
}

void set_error(char** destination, const char* message) noexcept {
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
        set_error(error, exception.what());
    } catch (...) {
        set_error(error, "CHA failed with an unknown error");
    }
}

bool environment_is_set(const char* name) {
    const char* const value = std::getenv(name);
    return value != nullptr && *value != '\0';
}

ApplicationCommand runtime_command(
    const char* config_path,
    const char* resource_path) {
    const char* arguments[] = {
        "CHA", "--root", resource_path, "--config", config_path};
    ApplicationCommand command = parse_application_command(5, arguments);
    // CHA.app does not use [web]. The only client is the WKWebView in this
    // process, so the listener is always private loopback on a port the
    // operating system picks; the section exists because the shared
    // configuration format requires it.
    command.host = "127.0.0.1";
    command.port = 0;
    return command;
}

int32_t transfer(
    ChaRuntime* runtime,
    uint64_t* byte_count,
    char** error,
    bool download) {
    clear_error(error);
    if (!runtime || !runtime->application || !byte_count) {
        set_error(error, "CHA runtime is not available");
        return 0;
    }
    try {
        const R2DatabaseTransfer result = download
            ? runtime->application->download_database()
            : runtime->application->upload_database();
        *byte_count = result.byte_count;
        return 1;
    } catch (const cha::WorkspaceRestartRequiredError& fatal) {
        set_error(error, fatal.what());
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
    if (!runtime || !runtime->application || !file_count) {
        set_error(error, "CHA runtime is not available");
        return 0;
    }
    try {
        const WorkspaceConfigTransfer result = importing
            ? runtime->application->import_configuration()
            : runtime->application->export_configuration();
        *file_count = result.file_count;
        return 1;
    } catch (const cha::WorkspaceRestartRequiredError& fatal) {
        set_error(error, fatal.what());
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
    char** error) {
    clear_error(error);
    if (!config_path || !resource_path || !access_token
        || *access_token == '\0') {
        set_error(error, "CHA runtime configuration is incomplete");
        return nullptr;
    }

    std::unique_ptr<ChaRuntime> runtime;
    try {
        ApplicationCommand command = runtime_command(
            config_path, resource_path);
        runtime = std::make_unique<ChaRuntime>();
        cha::initialize_diagnostic_logging(
            command.log_file, command.log_level);
        runtime->logging = true;
        runtime->application = ApplicationRuntime::open(
            command, access_token);
        runtime->can_modify = command.modify.has_value();
        runtime->port = runtime->application->start();
        return runtime.release();
    } catch (...) {
        if (runtime && runtime->logging) {
            cha::shutdown_diagnostic_logging();
        }
        set_current_error(error);
        return nullptr;
    }
}

void cha_runtime_destroy(ChaRuntime* runtime) {
    if (!runtime) return;
    if (runtime->application) {
        try {
            runtime->application->shutdown();
            runtime->application.reset();
        } catch (...) {
            // This boundary is called from Swift during application teardown;
            // no C++ exception may cross it. A failed shutdown intentionally
            // leaks the runtime until process exit rather than destroying
            // possibly live owner threads.
            (void)runtime->application.release();
        }
    }
    if (runtime->logging) cha::shutdown_diagnostic_logging();
    delete runtime;
}

int32_t cha_runtime_port(const ChaRuntime* runtime) {
    return runtime ? runtime->port : 0;
}

int32_t cha_runtime_can_modify(const ChaRuntime* runtime) {
    return runtime && runtime->can_modify ? 1 : 0;
}

int32_t cha_runtime_can_transfer_r2(const ChaRuntime* runtime) {
    return runtime
        && environment_is_set("CHA_R2_URL")
        && environment_is_set("CHA_R2_ACCESS_KEY_ID")
        && environment_is_set("CHA_R2_SECRET_ACCESS_KEY")
        ? 1 : 0;
}

int32_t cha_runtime_import_initial_database(
    const char* config_path,
    const char* seed_path,
    char** error) {
    clear_error(error);
    if (!config_path || !seed_path) {
        set_error(error, "CHA setup configuration is incomplete");
        return 0;
    }
    try {
        const char* arguments[] = {
            "CHA", "--config", config_path, "--import", seed_path};
        const ApplicationCommand command =
            parse_application_command(5, arguments);
        // The config file names the database, so the decision to seed one
        // belongs here rather than in a launcher that would have to guess.
        if (std::filesystem::is_regular_file(command.database)) return 1;
        const WorkspaceConfigTransfer result =
            import_workspace_configuration(
                *command.import_directory, command.database);
        (void)result;
        return 1;
    } catch (...) {
        set_current_error(error);
        return 0;
    }
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

void cha_string_free(char* value) {
    std::free(value);
}
