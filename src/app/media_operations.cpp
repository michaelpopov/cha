#include "app/media_operations.h"

#include "app/application.h"
#include "app/media_resources.h"
#include "util/curl.h"
#include "util/logging.h"
#include "util/text.h"
#include "web/fish_audio.h"

#include <curl/curl.h>

#include <stdexcept>
#include <utility>

namespace cha::app {
namespace {

using cha::web::AudioAcceptanceKind;
using cha::web::AudioJobState;
using cha::web::ErrorCode;

std::size_t receive_body(char* data, std::size_t size, std::size_t count, void* user) {
    const std::size_t bytes = size * count;
    auto& body = *static_cast<std::string*>(user);
    if (bytes > 256 * 1024 - body.size()) return 0;
    try {
        body.append(data, bytes);
    } catch (...) {
        return 0;
    }
    return bytes;
}

bool perform_transfer(CURL* curl, const char* error_buffer, const std::function<bool()>& cancelled) {
    const std::unique_ptr<CURLM, decltype(&curl_multi_cleanup)> multi(
        curl_multi_init(), curl_multi_cleanup);
    if (!multi) throw std::runtime_error("Could not create curl transfer");
    const auto require = [](CURLMcode result) {
        if (result != CURLM_OK) {
            throw std::runtime_error(
                "Voice input transfer failed (curl multi "
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
    if (!result || result->msg != CURLMSG_DONE) {
        throw std::runtime_error("Voice input transfer returned no result");
    }
    if (result->data.result != CURLE_OK) {
        throw std::runtime_error(
            std::string("Voice input request failed (curl ")
            + std::to_string(result->data.result) + "): "
            + curl_easy_strerror(result->data.result)
            + (error_buffer && *error_buffer
                ? std::string("; ") + error_buffer : std::string()));
    }
    return !cancelled();
}

const char* job_state_string(AudioJobState state) {
    switch (state) {
    case AudioJobState::queued: return "queued";
    case AudioJobState::running: return "running";
    case AudioJobState::failed: return "failed";
    }
    throw std::logic_error("Invalid audio job state.");
}

} // namespace

nlohmann::json media_resource_json(
    std::string_view resource_id,
    std::string_view mime_type,
    std::size_t byte_length) {
    return {
        {"resource_id", resource_id},
        {"url", MediaResources::url_for(resource_id)},
        {"mime_type", mime_type},
        {"byte_length", byte_length},
    };
}

nlohmann::json audio_acceptance_json(const cha::web::AudioAcceptance& acceptance) {
    nlohmann::json value{{"entry_id", acceptance.entry_id}};
    switch (acceptance.kind) {
    case AudioAcceptanceKind::cached:
        value["cached"] = true;
        break;
    case AudioAcceptanceKind::queued:
        value["cached"] = false;
        value["state"] = "queued";
        break;
    case AudioAcceptanceKind::running:
        value["cached"] = false;
        value["state"] = "running";
        break;
    }
    return value;
}

nlohmann::json audio_status_json(const cha::web::AudioDownloadStatus& status) {
    nlohmann::json pending = nlohmann::json::array();
    for (const auto& download : status.downloads) {
        nlohmann::json value{
            {"entry_id", download.entry_id},
            {"state", job_state_string(download.state)},
        };
        if (download.state == AudioJobState::failed) {
            value["error"] = "Audio download failed. Try again.";
        }
        pending.push_back(std::move(value));
    }
    return {
        {"cached_entry_ids", status.cached_entry_ids},
        {"downloads", std::move(pending)},
    };
}

void throw_audio_error(const cha::web::AudioDownloadError& error) {
    if (error.code == "vault_changed") {
        throw ApplicationError(ErrorCode::vault_changed, error.what());
    }
    if (error.code == "not_found") {
        throw ApplicationError(ErrorCode::not_found, error.what());
    }
    throw ApplicationError(ErrorCode::speech_busy, error.what());
}

void throw_speech_provider_error(long status, std::string_view body) {
    if (status >= 400 && cha::trim_view(body).empty()) {
        throw ApplicationError(
            ErrorCode::internal_error,
            cha::web::fish_audio_http_error_message(status));
    }
    throw ApplicationError(ErrorCode::internal_error, "FishAudio request failed.");
}

std::optional<std::string> connect_voice_transcription(
    std::string_view url,
    std::string_view key,
    std::string_view sdp,
    std::string_view model,
    std::string_view delay,
    std::string_view prompt,
    const std::vector<std::string>& languages,
    const std::function<bool()>& cancelled) {
    if (cancelled()) return std::nullopt;
    nlohmann::json transcription{
        {"model", model},
        {"prompt", prompt},
        {"delay", delay},
    };
    if (!languages.empty()) transcription["languages"] = languages;
    const nlohmann::json session = {
        {"type", "transcription"},
        {"audio", {
            {"input", {
                {"transcription", std::move(transcription)},
                {"turn_detection", nullptr},
            }},
        }},
    };
    const std::string session_body = session.dump();
    const std::string destination(url);
    const std::string offer(sdp);
    const std::string authorization = "Authorization: Bearer " + std::string(key);

    CurlHandle curl;
    char error_buffer[CURL_ERROR_SIZE]{};
    const auto require = [&](CURLcode result) {
        if (result != CURLE_OK) {
            throw std::runtime_error(
                std::string("Voice input request failed (curl ")
                + std::to_string(result) + "): " + curl_easy_strerror(result)
                + (*error_buffer ? std::string("; ") + error_buffer : std::string()));
        }
    };
    std::unique_ptr<curl_mime, decltype(&curl_mime_free)> mime(
        curl_mime_init(curl.get()), curl_mime_free);
    if (!mime) throw std::runtime_error("Could not create voice input request.");
    curl_mimepart* sdp_part = curl_mime_addpart(mime.get());
    require(curl_mime_name(sdp_part, "sdp"));
    require(curl_mime_data(sdp_part, offer.c_str(), CURL_ZERO_TERMINATED));
    curl_mimepart* session_part = curl_mime_addpart(mime.get());
    require(curl_mime_name(session_part, "session"));
    require(curl_mime_data(session_part, session_body.c_str(), CURL_ZERO_TERMINATED));

    CurlHeaders headers;
    headers.append(authorization);
    std::string answer;
    require(curl_easy_setopt(curl.get(), CURLOPT_ERRORBUFFER, error_buffer));
    require(curl_easy_setopt(curl.get(), CURLOPT_URL, destination.c_str()));
    require(curl_easy_setopt(curl.get(), CURLOPT_MIMEPOST, mime.get()));
    require(curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get()));
    require(curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, receive_body));
    require(curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &answer));
    require(curl_easy_setopt(curl.get(), CURLOPT_CONNECTTIMEOUT, 10L));
    require(curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, 30L));
    require(curl_easy_setopt(curl.get(), CURLOPT_NOSIGNAL, 1L));
    require(curl_easy_setopt(curl.get(), CURLOPT_FOLLOWLOCATION, 0L));
    if (!perform_transfer(curl.get(), error_buffer, cancelled)) return std::nullopt;
    long status = 0;
    require(curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &status));
    if (status < 200 || status >= 300) {
        throw ApplicationError(
            ErrorCode::internal_error,
            "The realtime transcription request failed.");
    }
    if (answer.empty()) {
        throw ApplicationError(
            ErrorCode::internal_error,
            "The realtime transcription request failed.");
    }
    return answer;
}

} // namespace cha::app
