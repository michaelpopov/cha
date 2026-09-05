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
    if (command.import_directory) {
        const WorkspaceConfigTransfer transferred = import_workspace_configuration(
            *command.import_directory, command.database);
        std::cout << "Imported " << transferred.file_count
                  << " files into '" << utf8_path(command.database) << "'\n";
        return 0;
    }
    if (command.export_directory) {
        const WorkspaceConfigTransfer transferred = export_workspace_configuration(
            command.database, *command.export_directory);
        std::cout << "Exported " << transferred.file_count
                  << " files to '" << utf8_path(*command.export_directory) << "'\n";
        return 0;
    }
    if (command.upload) {
        const R2DatabaseTransfer transferred = upload_database_to_r2(
            command.database);
        std::cout << "Uploaded " << transferred.byte_count
                  << " bytes from '" << utf8_path(command.database)
                  << "' to R2\n";
        return 0;
    }
    if (command.download) {
        const R2DatabaseTransfer transferred = download_database_from_r2(
            command.database);
        std::cout << "Downloaded " << transferred.byte_count
                  << " bytes from R2 into '" << utf8_path(command.database)
                  << "'\n";
        return 0;
    }

    initialize_diagnostic_logging(command.log_file, command.log_level);
    try {
        auto runtime = ApplicationRuntime::open(command);
        const int port = runtime->start();
        std::cout << "CHA ready at " << command.host << ':' << port << '\n'
                  << std::flush;
        runtime->wait_for_shutdown_signal();
    } catch (const std::exception& error) {
        // The log file is where a failed start is diagnosed, so it records the
        // reason and is flushed before main prints it.
        log_critical(
            std::string("web server event=failed reason=") + error.what());
        shutdown_diagnostic_logging();
        throw;
    }
    shutdown_diagnostic_logging();
    return 0;
}
