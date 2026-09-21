#pragma once

#include "media/audio_download.h"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace cha::app {

[[nodiscard]] nlohmann::json media_resource_json(
    std::string_view resource_id,
    std::string_view mime_type,
    std::size_t byte_length);
[[nodiscard]] nlohmann::json audio_acceptance_json(
    const AudioAcceptance& acceptance);
[[nodiscard]] nlohmann::json audio_status_json(
    const AudioDownloadStatus& status);

[[noreturn]] void throw_audio_error(const AudioDownloadError& error);
[[noreturn]] void throw_speech_provider_error(long status, std::string_view body);

// Authenticated realtime setup. JavaScript supplies only the SDP offer.
[[nodiscard]] std::optional<std::string> connect_voice_transcription(
    std::string_view url,
    std::string_view key,
    std::string_view sdp,
    std::string_view model,
    std::string_view delay,
    std::string_view prompt,
    const std::vector<std::string>& languages,
    const std::function<bool()>& cancelled);

} // namespace cha::app
