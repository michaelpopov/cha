#pragma once

#include "chat/character.h"
#include "util/text_template.h"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

// TOML table name for prompt-template variables and as
// TemplateOptions::scope_table_name when expanding character/forum prompts.
inline constexpr std::string_view prompt_scope_table = "prompt";

enum class Mode {
    net,
    test,
};

enum class ReasoningFormat {
    automatic,
    none,
    reasoning_content,
    reasoning,
};

// Which OpenAI-compatible wire protocol ProviderClient should speak.
enum class ProviderApi {
    chat_completions,
    responses,
};

// Hosted web-search policy for the Responses API. TOML spelling "auto" maps to
// automatic; Chat Completions rejects any value other than off.
enum class WebSearchMode {
    off,
    automatic,
    required,
};

enum class CacheRetention {
    off,
    short_,
    long_,
};

inline constexpr ProviderApi default_provider_api = ProviderApi::responses;
inline constexpr WebSearchMode default_web_search_mode = WebSearchMode::required;

// True only for the direct OpenAI API host, case-insensitively and allowing
// one DNS trailing dot. Gate cache-only wire extensions with this check.
bool is_direct_openai_host(std::string_view host);

// Effective private configuration for one model backend after the character's
// provider selection has been resolved.
struct ModelBackendConfig {
    std::string host;
    int port{};
    std::string base_path;
    Mode mode{Mode::test};
    std::string model;
    bool stream{true};
    std::optional<double> temperature;
    std::optional<int> max_tokens;
    int timeout_s{600};
    int idle_timeout_s{60};
    std::string api_key;
    std::string api_key_env;
    std::string reasoning_effort;
    ReasoningFormat reasoning_format{ReasoningFormat::automatic};
    bool https{};
    ProviderApi api{default_provider_api};
    WebSearchMode web_search{default_web_search_mode};
    CacheRetention cache_retention{CacheRetention::short_};
};

// Provider/runtime settings as written in one named provider configuration.
// Absent values fall back to the ModelBackendConfig defaults, not to another
// configuration file: a provider config is never merged with a second one.
// Validation errors are raised where the file is read, so no path is carried.
struct ProviderConfig {
    std::optional<std::string> host;
    std::optional<int> port;
    std::optional<std::string> base_path;
    std::optional<Mode> mode;
    std::optional<std::string> model;
    std::optional<bool> stream;
    std::optional<double> temperature;
    std::optional<int> max_tokens;
    std::optional<int> timeout_s;
    std::optional<int> idle_timeout_s;
    std::optional<std::string> api_key_env;
    std::optional<std::string> reasoning_effort;
    std::optional<ReasoningFormat> reasoning_format;
    std::optional<bool> https;
    std::optional<ProviderApi> api;
    std::optional<WebSearchMode> web_search;
    std::optional<CacheRetention> cache_retention;
};

// Materializes the effective provider values, leaving ModelBackendConfig's own
// defaults in place wherever a value is absent. Throws when required host or
// port values are missing; callers may validate first to provide richer context.
ModelBackendConfig make_backend_config(const ProviderConfig& effective);

// The character definition is the only layer allowed to select a provider.
// Forum defaults and member overrides still contribute prompt variables.
struct CharacterConfigPaths {
    std::optional<std::filesystem::path> providers_directory;
    std::optional<std::filesystem::path> styles_directory;
    std::filesystem::path definition;
    std::optional<std::filesystem::path> forum_defaults;
    std::optional<std::filesystem::path> member_override;
};

// Where a workspace keeps one config.toml per named provider.
std::filesystem::path providers_directory(const std::filesystem::path& workspace_root);
// Where a workspace keeps one config.toml per named character style.
std::filesystem::path styles_directory(const std::filesystem::path& workspace_root);

// The typed connection configuration and prompt scope after prompt layers.
struct LoadedCharacterConfig {
    CharacterMetadata character;
    ModelBackendConfig backend;
    TemplateScope prompt_variables;
};

// Resolves the provider selected by the definition, then merges prompt scope
// from the definition, forum defaults, and member override. Provider keys in
// the latter two files are deliberately ignored.
LoadedCharacterConfig load_character_config(const CharacterConfigPaths& paths);

// Loads definition-only identity and tag metadata. When providers_directory is
// supplied, validates any provider reference while loading metadata.
CharacterMetadata load_character_metadata(
    const std::filesystem::path& definition_path,
    std::optional<std::filesystem::path> providers_directory = std::nullopt,
    std::optional<std::filesystem::path> styles_directory = std::nullopt);

// Loads one named provider or style through the same parse the session runtime
// uses, so a catalog can drop an option that would not actually run.
ProviderConfig load_named_provider(
    const std::filesystem::path& directory,
    std::string_view name,
    const std::filesystem::path& reference_path);
CharacterAppearance load_named_style(
    const std::filesystem::path& directory,
    std::string_view name,
    const std::filesystem::path& reference_path);

} // namespace cha
