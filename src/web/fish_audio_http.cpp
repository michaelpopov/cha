#include "web/fish_audio.h"

#include "providers/api_key_store.h"
#include "util/logging.h"
#include "util/text.h"
#include "web/http_response.h"
#include "web/protocol.h"
#include "web/route_support.h"
#include "web/web_settings.h"
#include "workspace/workspace.h"

#include <httplib.h>

namespace cha::web {
namespace {
using Json = nlohmann::json;
}

void forward_fish_audio(
    const WorkspaceVoiceOutput& output, const std::string& key,
    const FishAudioRequest& request, httplib::Response& response,
    const std::function<bool()>& cancelled) {
    FishAudioProxy proxy;
    proxy.forward(output, key, request, response, cancelled);
}

void FishAudioProxy::forward(
    const WorkspaceVoiceOutput& output, const std::string& key,
    const FishAudioRequest& request, httplib::Response& response,
    const std::function<bool()>& cancelled) {
    const FishAudioTransfer result = synthesize(output, key, request, cancelled);
    if (result.busy) {
        set_error_response(
            response, 503,
            {ErrorCode::speech_busy, "Speech generation is busy. Try again shortly."});
        return;
    }
    if (result.cancelled) {
        set_error_response(
            response, 503,
            {ErrorCode::internal_error, "Speech generation cancelled."});
        return;
    }
    const long status = result.status;
    std::string audio = result.audio.audio;
    if (status >= 400 && trim_view(audio).empty()) {
        set_error_response(
            response, static_cast<int>(status),
            {ErrorCode::internal_error, fish_audio_http_error_message(status)});
    } else {
        response.status = static_cast<int>(status);
        response.set_content(
            std::move(audio),
            result.audio.content_type.empty() ? "audio/mpeg" : result.audio.content_type);
    }
    response.set_header("Cache-Control", "no-store");
}

void install_fish_audio_route(
    httplib::Server& server, ApiKeyStore& api_keys,
    const WebSettings& settings, bool voice_enabled, FishAudioProxy& proxy) {
    server.Post("/api/v1/voice-output/audio",
        [&api_keys, settings, voice_enabled, &proxy](
            const httplib::Request& request, httplib::Response& response) {
            if (!validate_json_mutation(request, response)) return;
            Json input;
            if (!parse_route_json_body(
                    request, response, settings.request_body_limit,
                    [&](const Json& parsed) {
                        if (parsed.contains("entry")) {
                            throw std::invalid_argument(
                                "Use the entry audio-download endpoint.");
                        }
                        input = parsed;
                    })) {
                return;
            }
            try {
                const auto workspace = getws();
                const auto* output = workspace && workspace->voice_output()
                    ? &*workspace->voice_output() : nullptr;
                if (!voice_enabled || !output || !api_keys.find(output->api_key_id)) {
                    set_route_not_found(response, "FishAudio output is not configured.");
                    return;
                }
                proxy.forward(
                    *output,
                    api_keys.value(output->api_key_id),
                    make_fish_audio_request(*output, input),
                    response,
                    request.is_connection_closed);
            } catch (const std::invalid_argument& error) {
                set_error_response(
                    response, 400, {ErrorCode::bad_request, error.what()});
            } catch (const std::exception& error) {
                log_warn(error.what());
                set_error_response(
                    response, 502,
                    {ErrorCode::internal_error, "FishAudio request failed."});
            }
        });
}
}
