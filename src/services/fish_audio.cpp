#include "services/fish_audio.h"

#include "util/curl.h"
#include "util/logging.h"
#include "util/text.h"
#include "workspace/workspace.h"
#include "session/session_repository.h"

#include <curl/curl.h>
#include <cmath>
#include <memory>
#include <regex>
#include <stdexcept>
#include <utility>

namespace cha {
namespace {
using Json = nlohmann::json;

std::size_t receive_audio(char* data, std::size_t size, std::size_t count, void* user) {
    const std::size_t bytes = size * count;
    auto& body = *static_cast<std::string*>(user);
    if (bytes > 256 * 1024 * 1024 - body.size()) return 0;
    try { body.append(data, bytes); } catch (...) { return 0; }
    return bytes;
}

void require_curl(CURLcode result, const char* detail = "") {
    if (result != CURLE_OK) {
        std::string message = "FishAudio request failed (curl " + std::to_string(result)
            + "): " + curl_easy_strerror(result);
        if (*detail) message += "; " + std::string(detail);
        throw std::runtime_error(message);
    }
}

bool perform_transfer(CURL* curl, const char* error_buffer, const std::function<bool()>& cancelled) {
    const std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)> multi(
        curl_multi_init(), curl_multi_cleanup);
    if (!multi) throw std::runtime_error("Could not create curl transfer");
    const auto require = [](CURLMcode result) {
        if (result != CURLM_OK) {
            throw std::runtime_error("FishAudio transfer failed (curl multi "
                + std::to_string(result) + "): " + curl_multi_strerror(result));
        }
    };
    require(curl_multi_add_handle(multi.get(), curl));
    const auto remove = [&](CURL* handle) { curl_multi_remove_handle(multi.get(), handle); };
    const std::unique_ptr<CURL, decltype(remove)> attached(curl, remove);
    int running = 0;
    do {
        if (cancelled()) return false;
        require(curl_multi_perform(multi.get(), &running));
        if (running) require(curl_multi_poll(multi.get(), nullptr, 0, 100, nullptr));
    } while (running);
    int remaining = 0;
    const CURLMsg* result = curl_multi_info_read(multi.get(), &remaining);
    if (!result || result->msg != CURLMSG_DONE) throw std::runtime_error("FishAudio transfer returned no result");
    require_curl(result->data.result, error_buffer);
    return !cancelled();
}
} // namespace

std::string entry_speech_text(const EntryAudioLookup& entry) {
    if (entry.entry_kind != EntryKind::character) return entry.entry_text;
    // Match the text shown in chat, including legacy echoed timestamps.
    static const std::regex timestamp_prefix(
        R"(^\s*\[[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]+)?Z\]\s*)");
    return std::regex_replace(remove_source_references(entry.entry_text), timestamp_prefix, "");
}

struct FishAudioResult { long status; EntryAudio audio; };

static std::optional<FishAudioResult> transfer_fish_audio(
    const WorkspaceVoiceOutput& output, const std::string& key,
    const FishAudioRequest& request,
    const std::function<bool()>& cancelled) {
    if (cancelled()) {
        return std::nullopt;
    }
    CurlHandle curl;
    CurlHeaders headers;
    for (const std::string& header : {
             std::string("Content-Type: application/json"),
             "Authorization: Bearer " + key, "model: " + request.model}) {
        headers.append(header);
    }
    char error_buffer[CURL_ERROR_SIZE]{};
    const auto require = [&](CURLcode result) { require_curl(result, error_buffer); };
    const std::string body = request.body.dump();
    std::string audio;
    require(curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, error_buffer));
    require(curl_easy_setopt(curl.get(), CURLOPT_URL, output.url.c_str()));
    require(curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, body.c_str()));
    require(curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get()));
    require(curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receive_audio));
    require(curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &audio));
    require(curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L));
    require(curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 180L));
    require(curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L));
    // Keep credentials on the configured endpoint, even if it redirects.
    require(curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L));
    if (!perform_transfer(curl.get(), error_buffer, cancelled)) {
        return std::nullopt;
    }
    long status = 0;
    char* content_type = nullptr;
    require(curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status));
    require(curl_easy_getinfo(curl.get(), CURLINFO_CONTENT_TYPE, &content_type));
    return FishAudioResult{status, {std::move(audio), content_type ? content_type : ""}};
}

std::optional<EntryAudio> download_fish_audio(
    const WorkspaceVoiceOutput& output, const std::string& key,
    const FishAudioRequest& request, const std::function<bool()>& cancelled) {
    auto result = transfer_fish_audio(output, key, request, cancelled);
    if (!result) return std::nullopt;
    if (result->status != 200) throw std::runtime_error("FishAudio request failed (HTTP " + std::to_string(result->status) + ").");
    auto& audio = result->audio;
    if (!valid_entry_audio(audio))
        throw std::runtime_error("FishAudio returned invalid audio.");
    return std::move(audio);
}

bool valid_entry_audio(const EntryAudio& audio) {
    // MIME types are case-insensitive and may include parameters. Opus and
    // other audio subtypes must work alongside MPEG, WAV, and Ogg.
    const auto type = trim_view(std::string_view(audio.content_type).substr(0, audio.content_type.find(';')));
    return !audio.audio.empty() && type.size() > 6 && starts_with_folded(type, "audio/");
}

std::string fish_audio_http_error_message(long status) {
    switch (status) {
    case 401: return "Authentication failed. Check the FishAudio API key.";
    case 403: return "Access denied by FishAudio.";
    case 402: return "Insufficient FishAudio credits.";
    case 413: return "Text is too large for FishAudio.";
    case 429: return "FishAudio rate limit reached. Try again shortly.";
    default: return "FishAudio request failed.";
    }
}

FishAudioTransfer FishAudioProxy::synthesize(
    const WorkspaceVoiceOutput& output, const std::string& key,
    const FishAudioRequest& request, const std::function<bool()>& cancelled) {
    if (!slots_.try_acquire()) {
        return {.busy = true};
    }
    struct ReleaseSlot {
        std::counting_semaphore<fish_audio_concurrency>& slots;
        ~ReleaseSlot() { slots.release(); }
    } release{slots_};
    auto result = transfer_fish_audio(
        output, key, request, [&] { return stopped_ || cancelled(); });
    if (!result) return {.cancelled = true};
    return {
        .status = result->status,
        .audio = std::move(result->audio),
    };
}

FishAudioSynthesis decode_fish_audio_synthesis(const Json& input) {
    FishAudioSynthesis synthesis;
    try {
        if (input.contains("reference_id")) {
            synthesis.reference_id = input.at("reference_id").get<std::string>();
        }
        const Json settings = input.value("settings", Json::object());
        if (!settings.is_object()) throw std::invalid_argument("Invalid voice settings");
        for (const auto& [name, value] : settings.items()) {
            if (name == "speed") {
                synthesis.settings.speed = value.get<double>();
            } else {
                synthesis.ignored_settings.push_back(name);
            }
        }
    } catch (const Json::exception&) {
        synthesis.decoding_failure = std::make_exception_ptr(std::invalid_argument("Invalid FishAudio request"));
    } catch (...) {
        synthesis.decoding_failure = std::current_exception();
    }
    return synthesis;
}

FishAudioRequest make_fish_audio_request(
    const WorkspaceVoiceOutput& output, std::string_view text, const FishAudioSynthesis& synthesis) {
    if (!synthesis.reference_id) throw std::invalid_argument("Invalid FishAudio request");
    if (text.empty() || synthesis.reference_id->empty()) throw std::invalid_argument("Missing text or voice ID");
    if (synthesis.decoding_failure) std::rethrow_exception(synthesis.decoding_failure);
    for (const auto& name : synthesis.ignored_settings) {
        log_warn("Ignoring unsupported FishAudio voice setting: " + name);
    }
    FishAudioRequest request{
        .model = output.model,
        .body = {{"text", text}, {"reference_id", *synthesis.reference_id}, {"format", output.output_format}},
    };
    if (synthesis.settings.speed) {
        const double speed = *synthesis.settings.speed;
        if (!std::isfinite(speed) || speed < 0.5 || speed > 2.0) throw std::invalid_argument("Invalid speed");
        request.body["prosody"] = {{"speed", speed}};
    }
    return request;
}

FishAudioRequest make_fish_audio_request(
    const WorkspaceVoiceOutput& output, const Json& input) try {
    const auto text = input.at("text").get<std::string>();
    return make_fish_audio_request(output, text, decode_fish_audio_synthesis(input));
} catch (const Json::exception&) {
    throw std::invalid_argument("Invalid FishAudio request");
}

}
