#pragma once

#include <string>
#include <string_view>

namespace cha {

// Appends validated JSON string content without opening or closing quotes.
void append_json_string_content(std::string& output, std::string_view text);

} // namespace cha
