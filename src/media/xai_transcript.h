#pragma once

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace cha::media {

inline constexpr std::string_view xai_malformed_transcript =
    "xAI returned a malformed transcript.";
inline constexpr std::string_view xai_missing_word_timings =
    "xAI returned a transcript without word timings.";
inline constexpr std::string_view xai_provider_error = "xAI voice input failed.";
inline constexpr std::size_t xai_max_pcm_bytes = 48 * 1024;
inline constexpr std::size_t xai_max_pending_text = 64 * 1024;
inline constexpr std::size_t xai_max_provider_message = 1024 * 1024;

class XaiTranscriptError : public std::runtime_error {
public:
    explicit XaiTranscriptError(std::string message);
};

struct XaiTranscriptUpdate {
    bool created = false;
    bool done = false;
    std::string addition;
};

// One cursor for the connection. Words are selected by end time only.
class XaiTranscriptNormalizer {
public:
    [[nodiscard]] XaiTranscriptUpdate apply(const nlohmann::json& event);
    [[nodiscard]] double last_committed_end() const noexcept;

private:
    [[nodiscard]] std::string consume_words(const nlohmann::json& event);

    double last_committed_end_ = -1;
    bool emitted_ = false;
    char last_char_ = '\0';
};

[[nodiscard]] std::string build_xai_stt_url(
    std::string_view endpoint,
    std::string_view model,
    const std::vector<std::string>& languages);

[[nodiscard]] bool curl_supports_websocket_scheme(std::string_view scheme);

// Even, nonempty PCM16. Rejects malformed and oversized input.
[[nodiscard]] std::vector<unsigned char> decode_pcm_base64(
    std::string_view encoded);

} // namespace cha::media
