#include "providers/api_key_store.h"
#include "util/environment.h"
#include "workspace/workspace_config_store.h"
#include "web/application_config.h"
#include "web/application_runtime.h"
#include "web/r2_database_transfer.h"
#include "util/logging.h"
#include "util/path_name.h"

#include <cstdio>
#include <cwchar>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <conio.h>
#include <io.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

using namespace cha;
using namespace web;

static int prepare_and_run(int argc, const char* argv[]);

static R2DatabaseTransfer transfer_r2(
    const ApplicationCommand& command,
    bool download,
    std::string_view database_password) {
    load_dotenv(command.config_directory / ".env");
    auto store = WorkspaceConfigStore::open(
        command.vault.data, std::string(database_password));
    ApiKeyStore keys(
        *store, command.config_directory / "api-keys.json");
    const std::optional<R2StorageKey> storage = keys.r2();
    if (!storage) throw std::runtime_error("The selected vault has no R2 key");
    auto maintenance = store->reserve_maintenance();
    maintenance.close();
    return download
        ? download_database_from_r2(
              command.vault.data,
              command.vault.source,
              *storage,
              R2DatabaseLease::already_held,
              database_password)
        : upload_database_to_r2(
              command.vault.data,
              command.vault.source,
              *storage,
              R2DatabaseLease::already_held,
              database_password);
}

static std::string request_vault_password(const ApplicationCommand& command) {
    if (!command.vault.password_protected) return {};
    std::cerr << "Password for vault '" << command.vault.name << "': "
              << std::flush;
#ifdef _WIN32
    std::wstring wide_password;
    bool read = false;
    if (::_isatty(::_fileno(stdin))) {
        for (;;) {
            const wint_t character = ::_getwch();
            if (character == '\r' || character == '\n') {
                read = true;
                std::cerr << '\n';
                break;
            }
            if (character == '\b') {
                if (!wide_password.empty()) wide_password.pop_back();
                continue;
            }
            if (character == 0 || character == 0xe0) {
                (void)::_getwch();
                continue;
            }
            if (character == WEOF) break;
            wide_password.push_back(static_cast<wchar_t>(character));
        }
        const std::string password = utf8_from_wide(wide_password);
        if (!read || password.empty()) {
            throw VaultPasswordError("Password required to open this vault");
        }
        return password;
    }
    std::string password;
    read = static_cast<bool>(std::getline(std::cin, password));
#else
    termios original{};
    const bool hide = ::isatty(STDIN_FILENO)
        && ::tcgetattr(STDIN_FILENO, &original) == 0;
    if (hide) {
        termios hidden = original;
        hidden.c_lflag &= static_cast<tcflag_t>(~ECHO);
        (void)::tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden);
    }
    std::string password;
    const bool read = static_cast<bool>(std::getline(std::cin, password));
    if (hide) {
        (void)::tcsetattr(STDIN_FILENO, TCSAFLUSH, &original);
        std::cerr << '\n';
    }
#endif
    if (!read || password.empty()) {
        throw VaultPasswordError("Password required to open this vault");
    }
    return password;
}

int main(int argc, const char* argv[]) {
    try {
        return prepare_and_run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "Failed: " << error.what() << '\n';
        return 1;
    }
}

int prepare_and_run(int argc, const char* argv[]) {
    const ApplicationCommand command = parse_application_command(argc, argv);
    const std::string vault_password = request_vault_password(command);
    initialize_diagnostic_logging(command.log_file, command.log_level);
    try {
        if (command.import_directory || command.export_directory
            || command.upload || command.download) {
            for (const std::string& warning : command.warnings) log_warn(warning);
        }
        if (command.import_directory) {
            const WorkspaceConfigTransfer transferred =
                import_workspace_configuration(
                    *command.import_directory,
                    command.vault.data,
                    WorkspaceConfigLease::acquire,
                    vault_password);
            std::cout << "Imported " << transferred.file_count
                      << " files into '" << utf8_path(command.vault.data) << "'\n";
        } else if (command.export_directory) {
            const WorkspaceConfigTransfer transferred =
                export_workspace_configuration(
                    command.vault.data,
                    *command.export_directory,
                    WorkspaceConfigLease::acquire,
                    vault_password);
            std::cout << "Exported " << transferred.file_count
                      << " files to '" << utf8_path(*command.export_directory) << "'\n";
        } else if (command.upload) {
            const R2DatabaseTransfer transferred = transfer_r2(
                command, false, vault_password);
            std::cout << "Uploaded " << transferred.byte_count
                      << " bytes from '" << utf8_path(command.vault.data)
                      << "' to R2\n";
        } else if (command.download) {
            const R2DatabaseTransfer transferred = transfer_r2(
                command, true, vault_password);
            std::cout << "Downloaded " << transferred.byte_count
                      << " bytes from R2 into '" << utf8_path(command.vault.data)
                      << "'\n";
        } else {
            auto runtime = ApplicationRuntime::open(
                command, {}, vault_password);
            const int port = runtime->start();
            std::cout << "CHA ready at " << command.host << ':' << port << '\n'
                      << std::flush;
            runtime->wait_for_shutdown_signal();
        }
    } catch (const std::exception& error) {
        log_critical(
            std::string("application event=failed reason=") + error.what());
        shutdown_diagnostic_logging();
        throw;
    }
    shutdown_diagnostic_logging();
    return 0;
}
