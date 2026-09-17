#include "characters/character_config.h"

#include "util/text.h"

#include <cmath>
#include <stdexcept>

namespace cha {

bool is_direct_openai_host(std::string_view host) {
    if (host.ends_with('.')) host.remove_suffix(1);
    return ascii_iequals(host, "api.openai.com");
}

bool is_openrouter_host(std::string_view host) {
    if (host.ends_with('.')) host.remove_suffix(1);
    return ascii_iequals(host, "openrouter.ai");
}

bool valid_openrouter_targets(const ModelBackendConfig& config) {
    if (config.openrouter_targets.empty()) return true;
    if (!is_openrouter_host(config.host)) return false;

    for (std::size_t index = 0;
         index < config.openrouter_targets.size();
         ++index) {
        const std::string& target = config.openrouter_targets[index];
        if (target.empty()) return false;
        for (const unsigned char value : target) {
            if (value <= 0x20 || value == 0x7f) return false;
        }
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            if (ascii_iequals(target, config.openrouter_targets[earlier])) {
                return false;
            }
        }
    }
    return true;
}

std::string_view to_string(Mode value) {
    switch (value) {
    case Mode::net: return "net";
    case Mode::test: return "test";
    }
    throw std::logic_error("Unknown provider mode");
}

std::string_view to_string(ProviderApi value) {
    switch (value) {
    case ProviderApi::chat_completions: return "chat_completions";
    case ProviderApi::responses: return "responses";
    }
    throw std::logic_error("Unknown provider API");
}

std::string_view to_string(ProviderAuth value) {
    switch (value) {
    case ProviderAuth::none: return "none";
    case ProviderAuth::openai_subscription: return "openai_subscription";
    }
    throw std::logic_error("Unknown provider auth");
}

std::string_view to_string(ReasoningFormat value) {
    switch (value) {
    case ReasoningFormat::automatic: return "auto";
    case ReasoningFormat::none: return "none";
    case ReasoningFormat::reasoning_content: return "reasoning_content";
    case ReasoningFormat::reasoning: return "reasoning";
    }
    throw std::logic_error("Unknown reasoning format");
}

std::string_view to_string(CacheRetention value) {
    switch (value) {
    case CacheRetention::off: return "off";
    case CacheRetention::short_: return "short";
    case CacheRetention::long_: return "long";
    }
    throw std::logic_error("Unknown cache retention");
}

std::optional<Mode> parse_mode(std::string_view value) {
    if (value == "net") return Mode::net;
    if (value == "test") return Mode::test;
    return std::nullopt;
}

std::optional<ProviderApi> parse_provider_api(std::string_view value) {
    if (value == "chat_completions") return ProviderApi::chat_completions;
    if (value == "responses") return ProviderApi::responses;
    return std::nullopt;
}

std::optional<ProviderAuth> parse_provider_auth(std::string_view value) {
    if (value == "none") return ProviderAuth::none;
    if (value == "openai_subscription") return ProviderAuth::openai_subscription;
    return std::nullopt;
}

std::optional<ReasoningFormat> parse_reasoning_format(std::string_view value) {
    if (value == "auto") return ReasoningFormat::automatic;
    if (value == "none") return ReasoningFormat::none;
    if (value == "reasoning_content") return ReasoningFormat::reasoning_content;
    if (value == "reasoning") return ReasoningFormat::reasoning;
    return std::nullopt;
}

std::optional<WebSearchMode> parse_web_search_mode(std::string_view value) {
    if (value == "off") return WebSearchMode::off;
    if (value == "auto") return WebSearchMode::automatic;
    if (value == "required") return WebSearchMode::required;
    return std::nullopt;
}

std::optional<CacheRetention> parse_cache_retention(std::string_view value) {
    if (value == "off") return CacheRetention::off;
    if (value == "short") return CacheRetention::short_;
    if (value == "long") return CacheRetention::long_;
    return std::nullopt;
}

bool provider_supports_web_search(const ModelBackendConfig& config) {
    if (config.auth == ProviderAuth::openai_subscription) return false;
    return config.api == ProviderApi::responses || is_openrouter_host(config.host);
}

std::optional<std::string_view> provider_config_error(
    const ModelBackendConfig& config) {
    if (config.host.empty()) {
        return "requires non-empty string 'host'";
    }
    if (config.model.empty()) {
        return "requires non-empty string 'model'";
    }
    if (!config.api_key_id.empty() && !config.api_key_env.empty()) {
        return "cannot set both api_key and api_key_env";
    }
    if (config.port < 1 || config.port > 65535) {
        return "requires port between 1 and 65535";
    }
    if (config.temperature
        && (!std::isfinite(*config.temperature)
            || *config.temperature < 0.0 || *config.temperature > 2.0)) {
        return "requires temperature between 0 and 2";
    }
    if (config.max_tokens && *config.max_tokens <= 0) {
        return "requires positive max_tokens";
    }
    if (config.timeout_s <= 0 || config.idle_timeout_s <= 0) {
        return "requires positive timeouts";
    }
    if (!config.base_path.empty()
        && (!config.base_path.starts_with('/')
            || config.base_path.ends_with('/')
            || config.base_path.find_first_of("?# \t\r\n") != std::string::npos)) {
        return "has invalid base_path";
    }
    if (!valid_openrouter_targets(config)) {
        return "has invalid OpenRouter inference targets";
    }
    if (config.web_search != WebSearchMode::off
        && !provider_supports_web_search(config)) {
        return "enables web search for an unsupported provider";
    }
    if (config.auth == ProviderAuth::openai_subscription) {
        if (config.host != "chatgpt.com"
            || config.port != 443
            || !config.https
            || config.base_path != "/backend-api/codex"
            || config.mode != Mode::net
            || config.api != ProviderApi::responses
            || !config.stream
            || !config.api_key_id.empty()
            || !config.api_key_env.empty()
            || config.temperature
            || config.max_tokens
            || config.web_search != WebSearchMode::off
            || config.cache_retention != CacheRetention::off) {
            return "has invalid openai_subscription settings";
        }
    }
    return std::nullopt;
}

std::string_view to_string(WebSearchMode value) {
    switch (value) {
    case WebSearchMode::off: return "off";
    case WebSearchMode::automatic: return "auto";
    case WebSearchMode::required: return "required";
    }
    throw std::logic_error("Unknown web search mode");
}

std::string provider_endpoint(const ModelBackendConfig& config) {
    if (config.auth == ProviderAuth::openai_subscription) {
        return "https://chatgpt.com/backend-api/codex/responses";
    }
    std::string host = config.host;
    if (host.find(':') != std::string::npos && !host.starts_with('[')) {
        host = '[' + host + ']';
    }
    const std::string base_url = std::string(config.https ? "https://" : "http://")
        + host + ':' + std::to_string(config.port) + config.base_path;
    switch (config.api) {
    case ProviderApi::chat_completions:
        return base_url + "/v1/chat/completions";
    case ProviderApi::responses:
        return base_url + "/v1/responses";
    }
    throw std::logic_error("Unknown provider API");
}

} // namespace cha
