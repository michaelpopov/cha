#include "providers/voice_output_config.h"
#include "util/text.h"

#include <curl/curl.h>

#include <algorithm>
#include <memory>
#include <stdexcept>

namespace cha {

std::string normalize_voice_output_connection(std::string_view value) {
    if (value != "http" && value != "websocket")
        throw std::invalid_argument("Voice connection must be HTTP or WebSocket.");
    return std::string(value);
}

bool elevenlabs_supports_speed(std::string_view model) {
    return !model.starts_with("eleven_v3") && !model.starts_with("eleven_v4");
}

void validate_voice_output_provider(std::string_view provider) {
    if (provider != "fishaudio" && provider != "elevenlabs")
        throw std::invalid_argument("Voice provider must be fishaudio or elevenlabs.");
}

std::string normalize_elevenlabs_output_format(std::string_view value) {
    const std::string format(trim_view(value));
    // Keep output playable by the browser; raw PCM has no container.
    static constexpr std::string_view formats[]{"mp3_22050_32", "mp3_24000_48",
        "mp3_44100_32", "mp3_44100_64", "mp3_44100_96", "mp3_44100_128", "mp3_44100_192",
        "opus_48000_32", "opus_48000_64", "opus_48000_96", "opus_48000_128", "opus_48000_192"};
    if (std::ranges::find(formats, format) == std::end(formats))
        throw std::invalid_argument("Unsupported ElevenLabs output format.");
    return format;
}

std::string parse_voice_output_endpoint(std::string_view value, std::string_view provider) {
    validate_voice_output_provider(provider);
    const std::string host = provider == "elevenlabs" ? "api.elevenlabs.io" : "api.fish.audio";
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
    if (fold_ascii(part(CURLUPART_HOST)) != host) {
        throw std::invalid_argument("Voice output endpoint must use " + host + ".");
    }
    if (scheme != "https") throw std::invalid_argument("Voice output requires an HTTPS URL.");
    require(curl_url_set(parsed.get(), CURLUPART_SCHEME, "https", 0));
    require(curl_url_set(parsed.get(), CURLUPART_HOST, host.c_str(), 0));
    if (part(CURLUPART_PATH) == "/") {
        require(curl_url_set(parsed.get(), CURLUPART_PATH, provider == "elevenlabs" ? "/v1/text-to-speech" : "/v1/tts", 0));
    }
    if (provider == "elevenlabs") {
        auto path = part(CURLUPART_PATH);
        if (path != "/v1/text-to-speech" && path != "/v1/text-to-speech/")
            throw std::invalid_argument("ElevenLabs endpoint must be https://api.elevenlabs.io/v1/text-to-speech.");
        for (const auto component : {CURLUPART_USER, CURLUPART_PASSWORD, CURLUPART_QUERY, CURLUPART_FRAGMENT}) {
            char* raw = nullptr;
            const auto result = curl_url_get(parsed.get(), component, &raw, 0);
            curl_free(raw);
            if (result == CURLUE_OK) throw std::invalid_argument("ElevenLabs endpoint cannot contain credentials, query, or fragment.");
        }
        require(curl_url_set(parsed.get(), CURLUPART_PATH, "/v1/text-to-speech", 0));
    }
    return part(CURLUPART_URL, CURLU_NO_DEFAULT_PORT);
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

std::string normalize_voice_output_format(std::string_view value) {
    const auto format = trim_view(value);
    if (format == "mp3" || format == "wav" || format == "opus") {
        return std::string(format);
    }
    // Old formats encoded a sample rate and bitrate after the container name.
    const auto separator = format.find('_');
    const auto container = format.substr(0, separator);
    if (separator != std::string_view::npos && (container == "mp3" || container == "opus")) {
        const auto parameters = format.substr(separator + 1);
        const auto bitrate = parameters.find('_');
        const auto digits = [](std::string_view part) {
            return !part.empty() && std::ranges::all_of(part, [](char c) {
                return c >= '0' && c <= '9';
            });
        };
        if (bitrate != std::string_view::npos && digits(parameters.substr(0, bitrate))
            && digits(parameters.substr(bitrate + 1))) {
            return std::string(container);
        }
    }
    throw std::invalid_argument("Output format must be mp3, wav, or opus.");
}

} // namespace cha
