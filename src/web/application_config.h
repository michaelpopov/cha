#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cha::web {

inline constexpr std::string_view default_text_to_speech_model =
    "eleven_multilingual_v2";

struct VaultDefinition {
    std::string name;
    std::filesystem::path data;
    std::optional<std::filesystem::path> mirror;
    std::optional<std::filesystem::path> modify;
    std::filesystem::path source;
};

struct VoiceInputConfig {
    std::string url{"https://api.openai.com/v1/realtime/calls"};
    std::string api_key_id;
    std::string model{"gpt-live-transcribe"};
    std::vector<std::string> languages;
    std::vector<std::string> keywords;
};

struct ConfigurationDirectory {
    std::filesystem::path directory;
    std::string startup_vault;
    std::vector<VaultDefinition> vaults;
    std::string host;
    int port{};
    std::filesystem::path log_file;
    std::string log_level;
    std::optional<VoiceInputConfig> voice_input;
    std::string text_to_speech_model{default_text_to_speech_model};
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
    // Test-only bound for vault-switch drain. Production always uses the
    // ordinary shutdown grace.
    std::optional<int> test_shutdown_grace_ms;
    std::optional<VoiceInputConfig> voice_input;
    std::string text_to_speech_model{default_text_to_speech_model};
    // Extra browser connection targets supplied only by a native shell. They
    // are not configurable by the served web application.
    std::vector<std::string> native_connect_urls;
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
