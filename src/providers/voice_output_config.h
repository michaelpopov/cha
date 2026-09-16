#pragma once

#include <string>
#include <string_view>

namespace cha {

std::string parse_voice_output_endpoint(std::string_view url);
std::string normalize_voice_output_model(std::string_view model);
std::string normalize_voice_output_format(std::string_view format);

} // namespace cha
