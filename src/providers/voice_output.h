#pragma once

#include "storage/session_repository.h"
#include "workspace/workspace.h"
#include <atomic>
#include <cstddef>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <nlohmann/json.hpp>

namespace cha {

inline constexpr std::size_t voice_output_concurrency = 4;

struct VoiceOutputRequest {
    std::string model;
    nlohmann::json body;
    std::string provider{"fishaudio"};
    std::string url;
};

using AudioChunkCallback = std::function<void(std::string_view mime_type, std::string_view bytes)>;

std::optional<EntryAudio> download_voice_output(
    const WorkspaceVoiceProviderOutput& output, const std::string& key,
    const VoiceOutputRequest& request, const std::function<bool()>& cancelled,
    const AudioChunkCallback& on_audio = {});
std::string entry_speech_text(const EntryAudioLookup& entry);
bool valid_entry_audio(const EntryAudio& audio);

// Browser shape errors are retained until new synthesis is actually prepared.
struct VoiceSynthesis {
    std::optional<std::string> reference_id;
    VoiceSettings settings;
    std::exception_ptr decoding_failure;
    std::vector<std::string> ignored_settings;
    std::string provider{"fishaudio"};
};
WorkspaceVoiceProviderOutput select_voice_output(const WorkspaceVoiceOutput& configured, std::string_view provider);
VoiceOutputRequest make_voice_output_request(
    const WorkspaceVoiceProviderOutput& output, std::string_view text, const VoiceSynthesis& synthesis);
std::string voice_output_http_error_message(
    std::string_view provider, long status, std::string_view body = {});

VoiceSynthesis decode_voice_synthesis(const nlohmann::json& input);
VoiceOutputRequest make_fish_audio_request(
    const WorkspaceVoiceProviderOutput& output, std::string_view text, const VoiceSynthesis& synthesis);
VoiceOutputRequest make_fish_audio_request(
    const WorkspaceVoiceProviderOutput& output, const nlohmann::json& input);

struct VoiceOutputTransfer {
    bool busy{};
    bool cancelled{};
    long status{};
    EntryAudio audio;
};

// Admission is immediate: synthesis must never queue on a request worker.
class VoiceOutputProxy {
public:
    void stop() { stopped_ = true; }
    VoiceOutputTransfer synthesize(
        const WorkspaceVoiceProviderOutput& output, const std::string& key,
        const VoiceOutputRequest& request, const std::function<bool()>& cancelled,
        const AudioChunkCallback& on_audio = {});

private:
    std::atomic_bool stopped_{false};
    std::atomic_size_t slots_{voice_output_concurrency};
};
}
