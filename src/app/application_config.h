#pragma once

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

class UnknownVaultError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class VaultPasswordError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct VaultDefinition {
    std::string name;
    std::filesystem::path data;
    bool password_protected{};
    std::optional<std::string> r2_etag;
    std::optional<std::filesystem::path> mirror;
    std::optional<std::filesystem::path> modify;
    std::filesystem::path source;
};

struct ConfigurationDirectory {
    std::filesystem::path directory;
    std::string startup_vault;
    std::optional<std::filesystem::path> mirror_base;
    std::optional<std::filesystem::path> modify_base;
    std::vector<VaultDefinition> vaults;
    std::filesystem::path log_file;
    std::string log_level;
    std::vector<std::string> warnings;
};

struct ApplicationCommand {
    std::filesystem::path config_directory;
    std::optional<std::filesystem::path> mirror_base;
    std::optional<std::filesystem::path> modify_base;
    std::vector<VaultDefinition> vaults;
    VaultDefinition vault;
    std::filesystem::path log_file;
    std::string log_level;
    std::vector<std::string> warnings;
    // Test-only bound for vault-switch drain. Production always uses the
    // ordinary shutdown grace.
    std::optional<int> test_shutdown_grace_ms;
};

ConfigurationDirectory load_configuration_directory(
    const std::filesystem::path& directory);
VaultDefinition load_vault_definition_file(
    const std::filesystem::path& configuration_directory,
    const std::filesystem::path& source);
bool same_vault_name(std::string_view left, std::string_view right);
const VaultDefinition* find_vault(
    const std::vector<VaultDefinition>& vaults, std::string_view name);
void validate_vault_definitions(
    const std::filesystem::path& directory,
    const std::vector<VaultDefinition>& vaults);
void require_switchable_database(
    const std::filesystem::path& database,
    std::string_view password = {});
void require_openable_protected_database(
    const std::filesystem::path& database,
    std::string_view password);
[[nodiscard]] std::optional<std::filesystem::path> session_mirror_root(
    const VaultDefinition& vault);

// Parses the public command line and its required configuration directory.
// Relative paths in that directory's files are resolved from the directory.
ApplicationCommand parse_application_command(
    int argc,
    const char* const* argv);

inline constexpr const char command_usage[] =
    "Usage:\n"
    "  CHA --config=CONFIG_DIR";

} // namespace cha
