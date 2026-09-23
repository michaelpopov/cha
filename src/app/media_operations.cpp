#include "app/media_operations.h"

#include "app/application_internal.h"
#include "media/media_resources.h"
#include "media/xai_transcript.h"
#include "media/xai_voice_session.h"
#include "app/settings_operations.h"
#include "util/curl.h"
#include "util/logging.h"
#include "util/text.h"
#include "providers/fish_audio.h"
#include "workspace/workspace.h"

#include <curl/curl.h>

#include <stdexcept>
#include <utility>

namespace cha::app {
namespace {

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

bool valid_xai_session_id(std::string_view session_id) {
    return !session_id.empty() && session_id.size() <= 64
        && session_id.find_first_not_of(
            "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_")
            == std::string_view::npos;
}

void validate_xai_languages(const std::vector<std::string>& languages) {
    if (languages.size() > 8) {
        throw ApplicationError(
            ErrorCode::invalid_argument, "The request was not valid.");
    }
    for (const std::string& language : languages) {
        if (language.size() > 16) {
            throw ApplicationError(
                ErrorCode::invalid_argument, "The request was not valid.");
        }
    }
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

nlohmann::json audio_acceptance_json(const AudioAcceptance& acceptance) {
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

nlohmann::json audio_status_json(const AudioDownloadStatus& status) {
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

void throw_audio_error(const AudioDownloadError& error) {
    if (error.code == "vault_changed") {
        throw ApplicationError(ErrorCode::vault_changed, error.what());
    }
    if (error.code == "not_found") {
        throw ApplicationError(ErrorCode::not_found, error.what());
    }
    throw ApplicationError(ErrorCode::speech_busy, error.what());
}

void throw_speech_provider_error(long status, std::string_view body) {
    if (status >= 400 && trim_view(body).empty()) {
        throw ApplicationError(
            ErrorCode::internal_error,
            fish_audio_http_error_message(status));
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

std::shared_ptr<OperationReply> Application::start_speech(
    std::string_view connection_id,
    std::uint64_t request_id,
    std::string text,
    FishAudioSynthesis synthesis,
    std::uint64_t epoch) {
    auto reply = std::make_shared<OperationReply>();
    WorkspaceVoiceOutput output;
    std::string key;
    FishAudioRequest request;
    std::shared_ptr<PendingMediaRegistry::PendingMedia> pending;
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        impl_->require_admitted(epoch);
        const auto workspace = impl_->store->snapshot();
        if (!workspace || !workspace->voice_output()
            || !impl_->api_keys->find(workspace->voice_output()->api_key_id)) {
            throw ApplicationError(
                ErrorCode::not_found, "Voice output is not configured.");
        }
        output = *workspace->voice_output();
        if (impl_->speech_url_override) output.url = *impl_->speech_url_override;
        if (!synthesis.reference_id) {
            const WorkspaceVoice* voice =
                workspace->find_voice_by_name(output.default_voice);
            if (!voice) {
                throw ApplicationError(
                    ErrorCode::not_found, "Voice output is not configured.");
            }
            synthesis.reference_id = voice->elevenlabs_voice_id;
        }
        key = impl_->api_keys->value(output.api_key_id);
        request = make_fish_audio_request(output, text, synthesis);
        pending = impl_->pending_media.remember(std::string(connection_id), request_id);
    }
    if (!impl_->background_jobs.launch(
            [owner = impl_.get(), reply, pending, output = std::move(output),
             key = std::move(key), request = std::move(request), epoch](
                std::atomic_bool& cancel) {
                PendingMediaRegistry::Cleanup cleanup{owner->pending_media, pending};
                const auto cancelled = [&] {
                    return cancel.load() || pending->cancelled->load();
                };
                try {
                    if (cancelled()) {
                        reply->fail(
                            ErrorCode::operation_cancelled,
                            "The operation was cancelled.");
                        return;
                    }
                    const auto transfer = owner->speech_proxy.synthesize(
                        output, key, request, cancelled);
                    if (cancelled() || transfer.cancelled) {
                        reply->fail(
                            ErrorCode::operation_cancelled,
                            "The operation was cancelled.");
                        return;
                    }
                    if (transfer.busy) {
                        reply->fail(
                            ErrorCode::speech_busy,
                            "Speech generation is busy. Try again shortly.");
                        return;
                    }
                    if (transfer.status != 200
                        || !valid_entry_audio(transfer.audio)) {
                        throw_speech_provider_error(
                            transfer.status, transfer.audio.audio);
                    }
                    std::string id;
                    {
                        const std::lock_guard lifecycle(owner->lifecycle_mutex);
                        if (cancelled()) {
                            reply->fail(
                                ErrorCode::operation_cancelled,
                                "The operation was cancelled.");
                            return;
                        }
                        if (const auto error = owner->admit_locked(epoch)) {
                            reply->fail(*error, {});
                            return;
                        }
                        id = owner->media_resources.add(
                            pending->connection_id,
                            epoch,
                            ResourceKind::speech,
                            {transfer.audio.content_type, transfer.audio.audio});
                        if (!owner->pending_media.set_resource(pending, id)) {
                            (void)owner->media_resources.release(
                                pending->connection_id, id);
                            reply->fail(
                                ErrorCode::operation_cancelled,
                                "The operation was cancelled.");
                            return;
                        }
                    }
                    if (!reply->complete(media_resource_json(
                        id, transfer.audio.content_type,
                        transfer.audio.audio.size()))) {
                        owner->pending_media.cancel(
                            pending->connection_id, pending->request_id);
                    }
                } catch (const ApplicationError& error) {
                    reply->fail(error.code, error.what());
                } catch (const std::invalid_argument& error) {
                    reply->fail(ErrorCode::invalid_argument, error.what());
                } catch (const std::exception& error) {
                    log_warn(error.what());
                    reply->fail(
                        ErrorCode::internal_error, "FishAudio request failed.");
                } catch (...) {
                    reply->fail(ErrorCode::internal_error, {});
                }
            })) {
        impl_->pending_media.forget(pending);
        reply->fail(
            ErrorCode::operation_cancelled, "The operation was cancelled.");
    }
    return reply;
}

void Application::cancel_speech(
    std::string_view connection_id,
    std::uint64_t request_id,
    std::uint64_t epoch) {
    if (const auto denied = check_context(epoch)) throw ApplicationError(*denied);
    impl_->pending_media.cancel(connection_id, request_id);
}

void Application::release_resource(
    std::string_view connection_id,
    std::string_view resource_id,
    std::uint64_t epoch) {
    if (const auto denied = check_context(epoch)) throw ApplicationError(*denied);
    impl_->pending_media.release_resource(connection_id, resource_id);
}

AudioAcceptance Application::start_audio(
    std::string_view forum_id,
    std::string_view session_id,
    EntryId entry_id,
    AudioDownloadRequest request,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    try {
        return impl_->audio_downloads->submit(
            {std::string(forum_id), std::string(session_id)},
            entry_id, request);
    } catch (const AudioDownloadError& error) {
        throw_audio_error(error);
    }
}

std::vector<AudioAcceptance> Application::start_audio_batch(
    std::string_view forum_id,
    std::string_view session_id,
    AudioDownloadBatchRequest request,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    try {
        return impl_->audio_downloads->submit_batch(
            {std::string(forum_id), std::string(session_id)}, request);
    } catch (const AudioDownloadError& error) {
        throw_audio_error(error);
    }
}

AudioDownloadStatus Application::audio_status(
    std::string_view forum_id,
    std::string_view session_id,
    std::string_view vault_name,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    try {
        return impl_->audio_downloads->status(
            {std::string(forum_id), std::string(session_id)},
            std::string(vault_name));
    } catch (const AudioDownloadError& error) {
        throw_audio_error(error);
    }
}

MediaResource Application::audio_source(
    std::string_view connection_id,
    std::string_view forum_id,
    std::string_view session_id,
    EntryId entry_id,
    std::string_view vault_name,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    std::optional<EntryAudio> audio;
    try {
        audio = impl_->audio_downloads->audio(
            {std::string(forum_id), std::string(session_id)},
            entry_id, std::string(vault_name));
    } catch (const AudioDownloadError& error) {
        throw_audio_error(error);
    }
    if (!audio) {
        throw ApplicationError(ErrorCode::not_found, "Cached audio not found.");
    }
    const std::string id = impl_->media_resources.add(
        connection_id,
        epoch,
        ResourceKind::entry_audio,
        {audio->content_type, audio->audio},
        FullSessionId{std::string(forum_id), std::string(session_id)},
        entry_id);
    return {
        id,
        MediaResources::url_for(id),
        audio->content_type,
        audio->audio.size(),
    };
}

void Application::clear_audio_cache(
    std::string_view forum_id,
    std::string_view session_id,
    std::uint64_t epoch) {
    const std::lock_guard lifecycle(impl_->lifecycle_mutex);
    impl_->require_admitted(epoch);
    const FullSessionId session{std::string(forum_id), std::string(session_id)};
    try {
        impl_->audio_downloads->clear(session);
    } catch (const AudioDownloadError& error) {
        throw_audio_error(error);
    }
    impl_->media_resources.revoke_session(session);
}

std::shared_ptr<OperationReply> Application::connect_voice_input(
    std::string_view connection_id,
    std::uint64_t request_id,
    std::string sdp,
    std::vector<std::string> languages,
    std::uint64_t epoch) {
    if (sdp.empty() || sdp.size() > 32768) {
        throw ApplicationError(
            ErrorCode::invalid_argument, "The request was not valid.");
    }
    if (languages.size() > 8) {
        throw ApplicationError(
            ErrorCode::invalid_argument, "The request was not valid.");
    }
    for (const std::string& language : languages) {
        if (language.empty() || language.size() > 16) {
            throw ApplicationError(
                ErrorCode::invalid_argument, "The request was not valid.");
        }
    }
    auto reply = std::make_shared<OperationReply>();
    std::string url;
    std::string key;
    std::string model;
    std::string delay;
    std::string prompt;
    std::shared_ptr<PendingMediaRegistry::PendingMedia> pending;
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        impl_->require_admitted(epoch);
        const auto secret = settings::voice_input_secret(
            *impl_->store->snapshot(), *impl_->api_keys, true);
        const auto runtime = settings::get_voice_input_runtime(
            *impl_->store->snapshot(), *impl_->api_keys, true);
        if (!secret || !runtime) {
            throw ApplicationError(
                ErrorCode::not_found, "Voice input is not configured.");
        }
        if (runtime->provider != "openai") {
            throw ApplicationError(
                ErrorCode::invalid_argument,
                "xAI voice input transport is not implemented");
        }
        url = runtime->url;
        key = *secret;
        model = runtime->model;
        delay = runtime->delay;
        prompt = runtime->prompt;
        pending = impl_->pending_media.remember(std::string(connection_id), request_id);
    }
    if (!impl_->background_jobs.launch(
            [owner = impl_.get(), reply, pending, url = std::move(url),
             key = std::move(key),
             model = std::move(model), delay = std::move(delay),
             prompt = std::move(prompt), sdp = std::move(sdp),
             languages = std::move(languages)](std::atomic_bool& cancel) {
                PendingMediaRegistry::Cleanup cleanup{owner->pending_media, pending};
                const auto cancelled = [&] {
                    return cancel.load() || pending->cancelled->load();
                };
                try {
                    if (cancelled()) {
                        reply->fail(
                            ErrorCode::operation_cancelled,
                            "The operation was cancelled.");
                        return;
                    }
                    auto answer = connect_voice_transcription(
                        url, key, sdp, model, delay, prompt, languages,
                        cancelled);
                    if (!answer || cancelled()) {
                        reply->fail(
                            ErrorCode::operation_cancelled,
                            "The operation was cancelled.");
                        return;
                    }
                    reply->complete({{"sdp", std::move(*answer)}});
                } catch (const ApplicationError& error) {
                    reply->fail(error.code, error.what());
                } catch (const std::exception& error) {
                    log_warn(error.what());
                    reply->fail(
                        ErrorCode::internal_error,
                        "The realtime transcription request failed.");
                } catch (...) {
                    reply->fail(ErrorCode::internal_error, {});
                }
            })) {
        impl_->pending_media.forget(pending);
        reply->fail(
            ErrorCode::operation_cancelled, "The operation was cancelled.");
    }
    return reply;
}

void Application::cancel_voice_input(
    std::string_view connection_id,
    std::uint64_t request_id,
    std::uint64_t epoch) {
    if (const auto denied = check_context(epoch)) throw ApplicationError(*denied);
    impl_->pending_media.cancel(connection_id, request_id);
}

std::shared_ptr<OperationReply> Application::start_xai_voice_input(
    std::string connection_id,
    std::uint64_t request_id,
    std::string session_id,
    std::vector<std::string> languages,
    std::uint64_t epoch,
    std::chrono::milliseconds deadline) {
    if (!valid_xai_session_id(session_id)) {
        throw ApplicationError(
            ErrorCode::invalid_argument, "The request was not valid.");
    }
    validate_xai_languages(languages);
    std::string endpoint;
    std::string model;
    std::string key;
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        impl_->require_admitted(epoch);
        const auto secret = settings::voice_input_secret(
            *impl_->store->snapshot(), *impl_->api_keys, true);
        const auto runtime = settings::get_voice_input_runtime(
            *impl_->store->snapshot(), *impl_->api_keys, true);
        if (!secret || !runtime) {
            throw ApplicationError(
                ErrorCode::not_found, "Voice input is not configured.");
        }
        if (runtime->provider != "xai") {
            throw ApplicationError(
                ErrorCode::invalid_argument, "Voice input provider is not xAI.");
        }
        endpoint = runtime->url;
        model = runtime->model;
        key = *secret;
    }
    const std::string url = media::build_xai_stt_url(endpoint, model, languages);
    return impl_->xai_voice.start(
        std::move(connection_id),
        request_id,
        std::move(session_id),
        epoch,
        url,
        "Authorization: Bearer " + key,
        deadline);
}

std::shared_ptr<OperationReply> Application::send_xai_voice_audio(
    std::string connection_id,
    std::uint64_t request_id,
    std::string session_id,
    std::string pcm_base64,
    std::uint64_t epoch,
    std::chrono::milliseconds deadline) {
    if (!valid_xai_session_id(session_id)) {
        throw ApplicationError(
            ErrorCode::invalid_argument, "The request was not valid.");
    }
    std::vector<unsigned char> pcm;
    try {
        pcm = media::decode_pcm_base64(pcm_base64);
    } catch (const std::invalid_argument&) {
        impl_->xai_voice.cancel(connection_id, session_id);
        throw ApplicationError(
            ErrorCode::invalid_argument, "The request was not valid.");
    }
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        impl_->require_admitted(epoch);
    }
    return impl_->xai_voice.audio(
        std::move(connection_id),
        request_id,
        std::move(session_id),
        std::move(pcm),
        deadline);
}

std::shared_ptr<OperationReply> Application::stop_xai_voice_input(
    std::string connection_id,
    std::uint64_t request_id,
    std::string session_id,
    std::int64_t remaining_ms,
    std::uint64_t epoch,
    std::chrono::milliseconds deadline) {
    if (!valid_xai_session_id(session_id) || remaining_ms < 0 || remaining_ms > 600000) {
        throw ApplicationError(
            ErrorCode::invalid_argument, "The request was not valid.");
    }
    {
        const std::lock_guard lifecycle(impl_->lifecycle_mutex);
        impl_->require_admitted(epoch);
    }
    return impl_->xai_voice.stop(
        std::move(connection_id),
        request_id,
        std::move(session_id),
        std::chrono::milliseconds(remaining_ms),
        deadline);
}

void Application::cancel_xai_voice_input(
    std::string_view connection_id,
    std::string_view session_id,
    std::uint64_t epoch) {
    if (const auto denied = check_context(epoch)) throw ApplicationError(*denied);
    impl_->xai_voice.cancel(connection_id, session_id);
}

void Application::expire_xai_voice_request(
    std::string_view connection_id,
    std::uint64_t request_id) {
    impl_->xai_voice.expire(connection_id, request_id);
}

void Application::set_xai_socket_factory_for_tests(
    std::function<std::unique_ptr<media::XaiSocket>()> factory) {
    impl_->xai_voice.set_socket_factory(std::move(factory));
}

std::optional<ResourceBytes> Application::read_resource(
    std::string_view connection_id,
    std::string_view resource_id) const {
    const std::unique_lock lifecycle(
        impl_->lifecycle_mutex, std::try_to_lock);
    if (!lifecycle.owns_lock()) return std::nullopt;
    const auto epoch = impl_->published_epoch.load();
    if (impl_->admit_locked(epoch)) return std::nullopt;
    return impl_->media_resources.read(
        connection_id, resource_id, epoch);
}

void Application::release_request_resources(
    std::string_view connection_id,
    std::uint64_t request_id) {
    impl_->pending_media.cancel(connection_id, request_id);
}

void Application::release_connection_resources(std::string_view connection_id) {
    impl_->pending_media.cancel_connection(connection_id);
    impl_->xai_voice.cancel_connection(connection_id);
}

void Application::set_speech_url_override(std::string url) {
    impl_->speech_url_override = std::move(url);
}

} // namespace cha::app
