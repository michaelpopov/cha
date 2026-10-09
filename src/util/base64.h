#pragma once

#include <string>
#include <string_view>

namespace cha {

std::string encode_base64(std::string_view bytes);
// Requires standard padded base64; throws on malformed input.
std::string decode_base64(std::string_view encoded);

} // namespace cha
