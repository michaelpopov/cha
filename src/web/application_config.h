#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cha::web {

struct VaultDefinition {
    std::string name;
    std::filesystem::path data;
    std::optional<std::filesystem::path> mirror;
    std::optional<std::filesystem::path> modify;
};

struct ConfigurationDirectory {
    std::filesystem::path directory;
    std::string startup_vault;
    std::vector<VaultDefinition> vaults;
    std::string host;
    int port{};
    std::filesystem::path log_file;
    std::string log_level;
};

struct ApplicationCommand {
    std::filesystem::path config_directory;
    std::vector<VaultDefinition> vaults;
    VaultDefinition vault;
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

ConfigurationDirectory load_configuration_directory(
    const std::filesystem::path& directory);
bool same_vault_name(std::string_view left, std::string_view right);
const VaultDefinition* find_vault(
    const std::vector<VaultDefinition>& vaults, std::string_view name);

// Parses the public command line and its required configuration directory.
// Relative paths in that directory's files are resolved from the directory.
ApplicationCommand parse_application_command(
    int argc,
    const char* const* argv);

// Customer-facing, so it omits --test-idle-grace-ms. That option shortens the
// runtime's idle unload so the browser suite can observe a real one; it is
// accepted but deliberately not advertised to someone who mistyped an option.
inline constexpr const char web_usage[] =
    "Usage:\n"
    "  chaweb --config=CONFIG_DIR [--root PATH]\n"
    "  chaweb --config=CONFIG_DIR --vault=NAME --import SOURCE_DIRECTORY\n"
    "  chaweb --config=CONFIG_DIR --vault=NAME --export DESTINATION_DIRECTORY\n"
    "  chaweb --config=CONFIG_DIR --vault=NAME --upload\n"
    "  chaweb --config=CONFIG_DIR --vault=NAME --download";

} // namespace cha::web
