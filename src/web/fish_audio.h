#pragma once

#include "session/session_repository.h"
#include "workspace/workspace.h"
#include <atomic>
#include <cstddef>
#include <exception>
#include <functional>
#include <optional>
#include <semaphore>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>

namespace httplib { class Server; struct Response; }
namespace cha {
class ApiKeyStore;
}
namespace cha::web {
struct WebSettings;

inline constexpr std::size_t fish_audio_concurrency = 4;

struct FishAudioRequest {
    std::string model;
    nlohmann::json body;
};

std::optional<EntryAudio> download_fish_audio(
    const WorkspaceVoiceOutput& output, const std::string& key,
    const FishAudioRequest& request, const std::function<bool()>& cancelled);
std::string entry_speech_text(const EntryAudioLookup& entry);
bool valid_entry_audio(const EntryAudio& audio);

// Browser shape errors are retained until new synthesis is actually prepared.
struct FishAudioSynthesis {
    std::optional<std::string> reference_id;
    VoiceSettings settings;
    std::exception_ptr decoding_failure;
    std::vector<std::string> ignored_settings;
};
FishAudioSynthesis decode_fish_audio_synthesis(const nlohmann::json& input);
FishAudioRequest make_fish_audio_request(
    const WorkspaceVoiceOutput& output, std::string_view text, const FishAudioSynthesis& synthesis);
FishAudioRequest make_fish_audio_request(
    const WorkspaceVoiceOutput& output, const nlohmann::json& input);
void forward_fish_audio(
    const WorkspaceVoiceOutput& output, const std::string& key,
    const FishAudioRequest& request, httplib::Response& response,
    const std::function<bool()>& cancelled = [] { return false; });

// Admission is immediate: synthesis must never queue on a request worker.
class FishAudioProxy {
public:
    void stop() { stopped_ = true; }
    void forward(
        const WorkspaceVoiceOutput& output, const std::string& key,
        const FishAudioRequest& request, httplib::Response& response,
        const std::function<bool()>& cancelled);

private:
    std::atomic_bool stopped_{false};
    std::counting_semaphore<fish_audio_concurrency> slots_{fish_audio_concurrency};
};

void install_fish_audio_route(
    httplib::Server& server, ApiKeyStore& api_keys,
    const WebSettings& settings, bool voice_enabled, FishAudioProxy& proxy);
}
