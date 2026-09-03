#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace cha {

enum class Mode { net, test };
enum class ReasoningFormat { automatic, none, reasoning_content, reasoning };
enum class ProviderApi { chat_completions, responses };
enum class WebSearchMode { off, automatic, required };
enum class CacheRetention { off, short_, long_ };

inline constexpr ProviderApi default_provider_api = ProviderApi::responses;
inline constexpr WebSearchMode default_web_search_mode =
    WebSearchMode::off;

bool is_direct_openai_host(std::string_view host);
bool is_openrouter_host(std::string_view host);
std::string_view to_string(WebSearchMode value);

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
    std::string api_key_env;
    std::string reasoning_effort;
    ReasoningFormat reasoning_format{ReasoningFormat::automatic};
    bool https{};
    ProviderApi api{default_provider_api};
    WebSearchMode web_search{default_web_search_mode};
    CacheRetention cache_retention{CacheRetention::short_};
};

std::string provider_endpoint(const ModelBackendConfig& config);

struct ProviderSelection {
    std::string id;
    ModelBackendConfig config;
};

} // namespace cha
