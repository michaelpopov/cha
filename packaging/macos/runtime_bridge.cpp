#include "runtime_bridge.h"

#include "util/logging.h"
#include "web/application_config.h"
#include "web/application_runtime.h"
#include "workspace/workspace_config_store.h"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <string>

using cha::WorkspaceConfigTransfer;
using cha::web::ApplicationCommand;
using cha::web::ApplicationRuntime;
using cha::web::R2DatabaseTransfer;
using cha::web::parse_application_command;

struct ChaRuntime {
    std::unique_ptr<ApplicationRuntime> application;
    std::optional<cha::web::VoiceInputConfig> voice_input;
    std::string voice_input_api_key;
    std::string text_to_speech_api_key;
    std::string text_to_speech_model;
    int port{};
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
#ifdef __APPLE__
    command.native_connect_urls.push_back("https://api.elevenlabs.io");
#endif
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
    const char* vault_password,
    int32_t* password_error,
    char** error) {
    clear_error(error);
    if (password_error) *password_error = 0;
    if (!config_path || !resource_path || !access_token || !vault_password
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
        runtime->voice_input = command.voice_input;
        runtime->text_to_speech_model = command.text_to_speech_model;
        runtime->application = ApplicationRuntime::open(
            command, access_token, vault_password);
        if (runtime->voice_input) {
            runtime->voice_input_api_key =
                runtime->application->api_key_value(
                    runtime->voice_input->api_key_id);
        }
#ifdef __APPLE__
        if (const auto key = runtime->application->api_key_value_by_name(
                "ELEVENLABS_API_KEY")) {
            runtime->text_to_speech_api_key = *key;
        }
#endif
        runtime->port = runtime->application->start();
        return runtime.release();
    } catch (const cha::web::VaultPasswordError&) {
        if (password_error) *password_error = 1;
        if (runtime && runtime->logging) {
            cha::shutdown_diagnostic_logging();
        }
        set_current_error(error);
        return nullptr;
    } catch (...) {
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
    char** error) {
    clear_error(error);
    if (!config_path || !resource_path) {
        set_error(error, "CHA runtime configuration is incomplete");
        return -1;
    }
    try {
        return runtime_command(config_path, resource_path)
                .vault.password_protected
            ? 1
            : 0;
    } catch (...) {
        set_current_error(error);
        return -1;
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
    if (!runtime || !runtime->application) return 0;
    try {
        return runtime->application->current_vault().modify ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

int32_t cha_runtime_can_transfer_r2(const ChaRuntime* runtime) {
    return runtime && runtime->application
        && runtime->application->has_r2_storage() ? 1 : 0;
}

const char* cha_runtime_voice_input_url(const ChaRuntime* runtime) {
    return runtime && runtime->voice_input
        ? runtime->voice_input->url.c_str() : nullptr;
}

const char* cha_runtime_voice_input_api_key(const ChaRuntime* runtime) {
    return runtime && runtime->voice_input
        ? runtime->voice_input_api_key.c_str() : nullptr;
}

const char* cha_runtime_voice_input_model(const ChaRuntime* runtime) {
    return runtime && runtime->voice_input
        ? runtime->voice_input->model.c_str() : nullptr;
}

int32_t cha_runtime_voice_input_language_count(const ChaRuntime* runtime) {
    return runtime && runtime->voice_input
        ? static_cast<int32_t>(runtime->voice_input->languages.size()) : 0;
}

const char* cha_runtime_voice_input_language(
    const ChaRuntime* runtime,
    int32_t index) {
    if (!runtime || !runtime->voice_input || index < 0
        || static_cast<std::size_t>(index)
            >= runtime->voice_input->languages.size()) {
        return nullptr;
    }
    return runtime->voice_input->languages[static_cast<std::size_t>(index)]
        .c_str();
}

int32_t cha_runtime_voice_input_keyword_count(const ChaRuntime* runtime) {
    return runtime && runtime->voice_input
        ? static_cast<int32_t>(runtime->voice_input->keywords.size()) : 0;
}

const char* cha_runtime_voice_input_keyword(
    const ChaRuntime* runtime,
    int32_t index) {
    if (!runtime || !runtime->voice_input || index < 0
        || static_cast<std::size_t>(index)
            >= runtime->voice_input->keywords.size()) {
        return nullptr;
    }
    return runtime->voice_input->keywords[static_cast<std::size_t>(index)]
        .c_str();
}

const char* cha_runtime_text_to_speech_api_key(const ChaRuntime* runtime) {
    return runtime && !runtime->text_to_speech_api_key.empty()
        ? runtime->text_to_speech_api_key.c_str() : nullptr;
}

const char* cha_runtime_text_to_speech_model(const ChaRuntime* runtime) {
    return runtime && !runtime->text_to_speech_model.empty()
        ? runtime->text_to_speech_model.c_str() : nullptr;
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
