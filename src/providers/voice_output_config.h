#pragma once

#include <string>
#include <string_view>

namespace cha {

void validate_voice_output_provider(std::string_view provider);
bool elevenlabs_supports_speed(std::string_view model);
std::string parse_voice_output_endpoint(std::string_view url, std::string_view provider = "fishaudio");
std::string normalize_elevenlabs_output_format(std::string_view format);
std::string normalize_voice_output_model(std::string_view model);
std::string normalize_voice_output_format(std::string_view format);

} // namespace cha
