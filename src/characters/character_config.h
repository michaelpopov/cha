#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

enum class Mode { net, test };
enum class ReasoningFormat { automatic, none, reasoning_content, reasoning };
enum class ProviderApi { chat_completions, responses };
enum class ProviderAuth { none, openai_subscription };
enum class WebSearchMode { off, automatic, required };
enum class CacheRetention { off, short_, long_ };

inline constexpr ProviderApi default_provider_api = ProviderApi::responses;
inline constexpr WebSearchMode default_web_search_mode =
    WebSearchMode::off;

bool is_direct_openai_host(std::string_view host);
bool is_openrouter_host(std::string_view host);
std::string_view mode_name(Mode value);
std::string_view api_name(ProviderApi value);
std::string_view auth_name(ProviderAuth value);
std::string_view reasoning_format_name(ReasoningFormat value);
std::string_view cache_retention_name(CacheRetention value);
std::string_view to_string(WebSearchMode value);

std::optional<Mode> parse_mode(std::string_view value);
std::optional<ProviderApi> parse_provider_api(std::string_view value);
std::optional<ProviderAuth> parse_provider_auth(std::string_view value);
std::optional<ReasoningFormat> parse_reasoning_format(std::string_view value);
std::optional<WebSearchMode> parse_web_search_mode(std::string_view value);
std::optional<CacheRetention> parse_cache_retention(std::string_view value);

// One request's resolved provider configuration. It is built from the current
// Workspace when generation starts and then owned by that request.
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
    std::string api_key_id;
    // Compatibility name resolved only through saved model keys. Despite the
    // legacy config spelling, the process environment is never consulted.
    std::string api_key_env;
    std::string reasoning_effort;
    ReasoningFormat reasoning_format{ReasoningFormat::automatic};
    bool https{};
    ProviderApi api{default_provider_api};
    ProviderAuth auth{ProviderAuth::none};
    WebSearchMode web_search{default_web_search_mode};
    CacheRetention cache_retention{CacheRetention::short_};
    // OpenRouter provider slugs tried in order. An empty list leaves routing to
    // OpenRouter; a non-empty list restricts fallback to these targets.
    std::vector<std::string> openrouter_targets;
};

bool valid_openrouter_targets(const ModelBackendConfig& config);
bool provider_supports_web_search(const ModelBackendConfig& config);
// Value rules only; callers supply file/request context and validate references.
std::optional<std::string_view> provider_config_error(
    const ModelBackendConfig& config);
std::string provider_endpoint(const ModelBackendConfig& config);

struct ProviderSelection {
    std::string id;
    ModelBackendConfig config;
};

} // namespace cha
