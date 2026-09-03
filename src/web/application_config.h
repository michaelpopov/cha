#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace cha::web {

struct ApplicationCommand {
    std::filesystem::path database;
    std::optional<std::filesystem::path> mirror;
    std::optional<std::filesystem::path> import_directory;
    std::optional<std::filesystem::path> export_directory;
    bool upload{};
    bool download{};
    std::filesystem::path root;
    std::string host;
    int port{};
    std::filesystem::path log_file;
    std::string log_level;
    // Browser automation uses this command-line-only seam to prove a real
    // disconnect/unload/reopen cycle without adding thirty seconds per run.
    std::optional<int> test_idle_grace_ms;
};

// Parses the public command line and its required external TOML file. Relative
// data and logging paths are resolved from the configuration file's directory.
ApplicationCommand parse_application_command(
    int argc,
    const char* const* argv);

// Customer-facing, so it omits --test-idle-grace-ms. That option shortens the
// runtime's idle unload so the browser suite can observe a real one; it is
// accepted but deliberately not advertised to someone who mistyped an option.
inline constexpr const char web_usage[] =
    "Usage:\n"
    "  chaweb --config=CONFIG [--root PATH]\n"
    "  chaweb --config=CONFIG --import SOURCE_DIRECTORY\n"
    "  chaweb --config=CONFIG --export DESTINATION_DIRECTORY\n"
    "  chaweb --config=CONFIG --upload\n"
    "  chaweb --config=CONFIG --download";

} // namespace cha::web
