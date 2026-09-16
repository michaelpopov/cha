#include "providers/voice_output_config.h"
#include "util/text.h"

#include <curl/curl.h>

#include <algorithm>
#include <memory>
#include <stdexcept>

namespace cha {

VoiceOutputEndpoint parse_voice_output_endpoint(std::string_view value) {
    const std::string url(trim_view(value));
    const std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> parsed(
        curl_url(), curl_url_cleanup);
    const auto require = [](CURLUcode result) {
        if (result != CURLUE_OK) {
            throw std::invalid_argument("Output URL must be an absolute HTTP or HTTPS URL.");
        }
    };
    if (!parsed) throw std::runtime_error("Could not parse voice output URL");
    require(curl_url_set(parsed.get(), CURLUPART_URL, url.c_str(), 0));
    const auto part = [&](CURLUPart name, unsigned flags = 0) {
        char* raw = nullptr;
        require(curl_url_get(parsed.get(), name, &raw, flags));
        const std::unique_ptr<char, decltype(&curl_free)> owned(raw, curl_free);
        return std::string(raw);
    };
    const std::string scheme = fold_ascii(part(CURLUPART_SCHEME));
    if (scheme != "http" && scheme != "https") {
        throw std::invalid_argument("Output URL must be an absolute HTTP or HTTPS URL.");
    }
    const bool fish_audio = fold_ascii(part(CURLUPART_HOST)) == "api.fish.audio";
    if (!fish_audio) return {url, false};
    if (scheme != "https") throw std::invalid_argument("FishAudio requires an HTTPS URL.");
    require(curl_url_set(parsed.get(), CURLUPART_SCHEME, "https", 0));
    require(curl_url_set(parsed.get(), CURLUPART_HOST, "api.fish.audio", 0));
    if (part(CURLUPART_PATH) == "/") {
        require(curl_url_set(parsed.get(), CURLUPART_PATH, "/v1/tts", 0));
    }
    return {part(CURLUPART_URL, CURLU_NO_DEFAULT_PORT), true};
}

std::string normalize_voice_output_model(std::string_view value) {
    const std::string model(trim_view(value));
    if (model.empty() || std::ranges::any_of(model, [](unsigned char character) {
            return character < 32 || character == 127;
        })) {
        throw std::invalid_argument("Output model must be nonempty and contain no control characters.");
    }
    return model;
}

} // namespace cha
