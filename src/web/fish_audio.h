#pragma once

#include <atomic>
#include <cstddef>
#include <functional>
#include <semaphore>
#include <string>
#include <nlohmann/json.hpp>

namespace httplib { class Server; struct Response; }
namespace cha {
class ApiKeyStore;
class SessionRepository;
struct WorkspaceVoiceOutput;
}
namespace cha::web {
struct WebSettings;

inline constexpr std::size_t fish_audio_concurrency = 4;

struct FishAudioRequest {
    std::string model;
    nlohmann::json body;
};

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
    httplib::Server& server, ApiKeyStore& api_keys, const SessionRepository& sessions,
    const WebSettings& settings, bool native_voice_enabled, FishAudioProxy& proxy);
}
