#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace cha::web {

struct ApplicationCommand {
    std::filesystem::path database;
    std::optional<std::filesystem::path> import_directory;
    std::optional<std::filesystem::path> export_directory;
    std::filesystem::path root;
    std::optional<std::string> host;
    std::optional<int> port;
    // Browser automation uses this command-line-only seam to prove a real
    // disconnect/unload/reopen cycle without adding thirty seconds per run.
    std::optional<int> test_idle_grace_ms;
};

struct StoredApplicationSettings {
    std::string host;
    int port{};
};

// Reads materialized root app.toml. host and port are required. The stored
// form does not accept workspace or backup_dir.
StoredApplicationSettings load_stored_application_settings(
    const std::filesystem::path& config_file);

// Parses the public command line. Does not read stored app.toml; runtime
// loads host and port from the materialized database after startup.
ApplicationCommand parse_application_command(
    int argc,
    const char* const* argv);

// Customer-facing, so it omits --test-idle-grace-ms. That option shortens the
// runtime's idle unload so the browser suite can observe a real one; it is
// accepted but deliberately not advertised to someone who mistyped an option.
inline constexpr const char web_usage[] =
    "Usage:\n"
    "  chaweb --data DATABASE [--root PATH] [--host HOST] [--port PORT]\n"
    "  chaweb --data DATABASE --import SOURCE_DIRECTORY\n"
    "  chaweb --data DATABASE --export DESTINATION_DIRECTORY";

} // namespace cha::web
