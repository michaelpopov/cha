#include "characters/character_config.h"

#include "util/text.h"

#include <stdexcept>

namespace cha {

bool is_direct_openai_host(std::string_view host) {
    if (host.ends_with('.')) host.remove_suffix(1);
    return ascii_iequals(host, "api.openai.com");
}

std::string provider_endpoint(const ModelBackendConfig& config) {
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
