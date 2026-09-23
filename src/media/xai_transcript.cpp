#include "media/xai_transcript.h"

#include "util/curl.h"
#include "util/logging.h"
#include "util/text.h"

#include <curl/curl.h>

#include <cmath>
#include <limits>
#include <stdexcept>
#include <utility>

namespace cha::media {
namespace {

bool blank_text(const nlohmann::json& event) {
    if (!event.contains("text") || event["text"].is_null()) return true;
    if (!event["text"].is_string()) {
        throw XaiTranscriptError(std::string(xai_malformed_transcript));
    }
    return trim_view(event["text"].get<std::string>()).empty();
}

bool starts_with_separator(std::string_view text) {
    return !text.empty()
        && std::string_view(",.;:!?)}]").find(text.front())
            != std::string_view::npos;
}

int base64_digit(char character) {
    if (character >= 'A' && character <= 'Z') return character - 'A';
    if (character >= 'a' && character <= 'z') return character - 'a' + 26;
    if (character >= '0' && character <= '9') return character - '0' + 52;
    if (character == '+') return 62;
    if (character == '/') return 63;
    return -1;
}

} // namespace

XaiTranscriptError::XaiTranscriptError(std::string message)
    : std::runtime_error(std::move(message)) {}

double XaiTranscriptNormalizer::last_committed_end() const noexcept {
    return last_committed_end_;
}

XaiTranscriptUpdate XaiTranscriptNormalizer::apply(const nlohmann::json& event) {
    if (!event.is_object() || !event.contains("type") || !event["type"].is_string()) {
        throw XaiTranscriptError(std::string(xai_malformed_transcript));
    }
    const std::string type = event["type"].get<std::string>();
    if (type == "transcript.created") {
        XaiTranscriptUpdate update;
        update.created = true;
        return update;
    }
    if (type == "error") {
        throw XaiTranscriptError(std::string(xai_provider_error));
    }
    if (type != "transcript.partial" && type != "transcript.done") return {};
    if (type == "transcript.partial") {
        if (!event.contains("is_final") || !event["is_final"].is_boolean()
            || !event.contains("speech_final") || !event["speech_final"].is_boolean()) {
            throw XaiTranscriptError(std::string(xai_malformed_transcript));
        }
        if (!event["is_final"].get<bool>()) return {};
    }
    XaiTranscriptUpdate update;
    update.done = type == "transcript.done";
    update.addition = consume_words(event);
    return update;
}

std::string XaiTranscriptNormalizer::consume_words(const nlohmann::json& event) {
    if (event.contains("text") && !event["text"].is_null()
        && !event["text"].is_string()) {
        throw XaiTranscriptError(std::string(xai_malformed_transcript));
    }
    const bool has_words = event.contains("words") && !event["words"].is_null();
    const bool words_empty = !has_words
        || (event["words"].is_array() && event["words"].empty());
    if (words_empty) {
        if (blank_text(event)) return {};
        throw XaiTranscriptError(std::string(xai_missing_word_timings));
    }
    if (!event["words"].is_array()) {
        throw XaiTranscriptError(std::string(xai_malformed_transcript));
    }

    struct Word {
        std::string text;
        double end = 0;
    };
    std::vector<Word> words;
    double previous_end = 0;
    bool have_previous = false;
    for (const auto& item : event["words"]) {
        if (!item.is_object() || !item.contains("text") || !item["text"].is_string()
            || !item.contains("start") || !item.contains("end")
            || !item["start"].is_number() || !item["end"].is_number()) {
            throw XaiTranscriptError(std::string(xai_malformed_transcript));
        }
        const double start = item["start"].get<double>();
        const double end = item["end"].get<double>();
        if (!std::isfinite(start) || !std::isfinite(end) || start < 0 || end < 0
            || start > end || (have_previous && end < previous_end)) {
            throw XaiTranscriptError(std::string(xai_malformed_transcript));
        }
        previous_end = end;
        have_previous = true;
        words.push_back(Word{item["text"].get<std::string>(), end});
    }

    const double cursor = last_committed_end_;
    std::string addition;
    double max_end = cursor;
    bool selected = false;
    for (const Word& word : words) {
        if (!(word.end > cursor)) continue;
        selected = true;
        if (word.end > max_end) max_end = word.end;
        const std::string text(trim_view(word.text));
        if (text.empty()) continue;
        if (emitted_ && !is_space(last_char_) && !starts_with_separator(text)) {
            addition.push_back(' ');
        }
        addition += text;
        last_char_ = text.back();
        emitted_ = true;
    }
    if (selected) last_committed_end_ = max_end;
    return addition;
}

std::string build_xai_stt_url(
    std::string_view endpoint,
    std::string_view model,
    const std::vector<std::string>& languages) {
    std::string base(endpoint);
    const auto query = base.find_first_of("?#");
    if (query != std::string::npos) {
        log_warn("Ignored query on the xAI voice input endpoint.");
        base.resize(query);
    }
    CurlHandle curl;
    const auto escape = [&](std::string_view value) {
        char* encoded = curl_easy_escape(
            curl.get(),
            value.data(),
            static_cast<int>(value.size()));
        if (!encoded) {
            throw std::runtime_error("Could not encode the xAI voice input request.");
        }
        std::string result(encoded);
        curl_free(encoded);
        return result;
    };
    std::string url = base;
    url += "?model=";
    url += escape(model);
    url += "&encoding=pcm&sample_rate=16000&channels=1";
    url += "&interim_results=false&endpointing=400";
    for (const std::string& language : languages) {
        if (language.empty()) continue;
        url += "&language=";
        url += escape(language);
        break;
    }
    return url;
}

bool curl_supports_websocket_scheme(std::string_view scheme) {
    const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    if (!info || !info->protocols) return false;
    for (const char* const* protocol = info->protocols; *protocol; ++protocol) {
        if (scheme == *protocol) return true;
    }
    return false;
}

std::vector<unsigned char> decode_pcm_base64(std::string_view encoded) {
    if (encoded.empty() || encoded.size() > 65536 || encoded.size() % 4 != 0
        || encoded.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("The request was not valid.");
    }
    std::vector<unsigned char> pcm;
    pcm.reserve(encoded.size() / 4 * 3);
    for (std::size_t index = 0; index < encoded.size(); index += 4) {
        int values[4] = {};
        int padding = 0;
        for (int part = 0; part < 4; ++part) {
            const char character = encoded[index + static_cast<std::size_t>(part)];
            if (character == '=') {
                // Padding is valid only in the last group.
                if (part < 2 || index + 4 != encoded.size()) {
                    throw std::invalid_argument("The request was not valid.");
                }
                ++padding;
                continue;
            }
            if (padding != 0) throw std::invalid_argument("The request was not valid.");
            values[part] = base64_digit(character);
            if (values[part] < 0) throw std::invalid_argument("The request was not valid.");
        }
        pcm.push_back(static_cast<unsigned char>(
            (values[0] << 2) | (values[1] >> 4)));
        if (padding < 2) {
            pcm.push_back(static_cast<unsigned char>(
                ((values[1] & 0x0f) << 4) | (values[2] >> 2)));
        }
        if (padding < 1) {
            pcm.push_back(static_cast<unsigned char>(
                ((values[2] & 0x03) << 6) | values[3]));
        }
    }
    if (pcm.empty() || pcm.size() % 2 != 0 || pcm.size() > xai_max_pcm_bytes) {
        throw std::invalid_argument("The request was not valid.");
    }
    return pcm;
}

} // namespace cha::media
