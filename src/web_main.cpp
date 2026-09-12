#include "providers/api_key_store.h"
#include "util/environment.h"
#include "workspace/workspace_config_store.h"
#include "web/application_config.h"
#include "web/application_runtime.h"
#include "web/r2_database_transfer.h"
#include "util/logging.h"
#include "util/path_name.h"

#include <exception>
#include <iostream>
#include <string>

using namespace cha;
using namespace web;

static int prepare_and_run(int argc, const char* argv[]);

static R2DatabaseTransfer transfer_r2(
    const ApplicationCommand& command,
    bool download) {
    load_dotenv(command.config_directory / ".env");
    auto store = WorkspaceConfigStore::open(command.vault.data);
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
              R2DatabaseLease::already_held)
        : upload_database_to_r2(
              command.vault.data,
              command.vault.source,
              *storage,
              R2DatabaseLease::already_held);
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
    initialize_diagnostic_logging(command.log_file, command.log_level);
    try {
        if (command.import_directory || command.export_directory
            || command.upload || command.download) {
            for (const std::string& warning : command.warnings) log_warn(warning);
        }
        if (command.import_directory) {
            const WorkspaceConfigTransfer transferred =
                import_workspace_configuration(
                    *command.import_directory, command.vault.data);
            std::cout << "Imported " << transferred.file_count
                      << " files into '" << utf8_path(command.vault.data) << "'\n";
        } else if (command.export_directory) {
            const WorkspaceConfigTransfer transferred =
                export_workspace_configuration(
                    command.vault.data, *command.export_directory);
            std::cout << "Exported " << transferred.file_count
                      << " files to '" << utf8_path(*command.export_directory) << "'\n";
        } else if (command.upload) {
            const R2DatabaseTransfer transferred = transfer_r2(command, false);
            std::cout << "Uploaded " << transferred.byte_count
                      << " bytes from '" << utf8_path(command.vault.data)
                      << "' to R2\n";
        } else if (command.download) {
            const R2DatabaseTransfer transferred = transfer_r2(command, true);
            std::cout << "Downloaded " << transferred.byte_count
                      << " bytes from R2 into '" << utf8_path(command.vault.data)
                      << "'\n";
        } else {
            auto runtime = ApplicationRuntime::open(command);
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
