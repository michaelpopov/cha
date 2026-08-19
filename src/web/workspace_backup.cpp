#include "web/workspace_backup.h"

#include "util/path_name.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <stdexcept>
#include <string>

namespace cha::web {
namespace {

std::string shell_quote(const std::filesystem::path& path) {
    std::string value = utf8_path(path);
    std::string result = "'";
    for (const char character : value) {
        if (character == '\'') result += "'\\''";
        else result += character;
    }
    return result + "'";
}

std::string backup_file_name() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &now) != 0) {
#else
    if (localtime_r(&now, &local) == nullptr) {
#endif
        throw std::runtime_error("Could not determine the current time for workspace backup.");
    }
    char buffer[sizeof "chaweb-2000-01-01-00-00.tar.gz"];
    if (std::strftime(buffer, sizeof buffer, "chaweb-%Y-%m-%d-%H-%M.tar.gz", &local) == 0) {
        throw std::runtime_error("Could not format the workspace backup name.");
    }
    return buffer;
}

} // namespace

std::filesystem::path backup_workspace(
    const std::filesystem::path& workspace,
    const std::filesystem::path& backup_dir) {
    std::error_code error;
    std::filesystem::create_directories(backup_dir, error);
    if (error) {
        throw std::runtime_error(
            "Could not create backup directory '" + utf8_path(backup_dir)
            + "': " + error.message());
    }

    const std::filesystem::path archive = backup_dir / backup_file_name();
    const std::string command = "tar cfz " + shell_quote(archive)
        + " -C " + shell_quote(workspace.parent_path())
        + " " + shell_quote(workspace.filename());
    if (std::system(command.c_str()) != 0) {
        throw std::runtime_error(
            "Could not create workspace backup '" + utf8_path(archive) + "'.");
    }
    return archive;
}

} // namespace cha::web
