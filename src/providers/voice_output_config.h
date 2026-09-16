#pragma once

#include <string>
#include <string_view>

namespace cha {

struct VoiceOutputEndpoint {
    std::string url;
    bool fish_audio;
};

VoiceOutputEndpoint parse_voice_output_endpoint(std::string_view url);
std::string normalize_voice_output_model(std::string_view model);

} // namespace cha
