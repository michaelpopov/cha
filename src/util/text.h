#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace cha {

bool is_space(char character);
std::size_t find_whitespace(std::string_view value);
std::string_view trim_view(std::string_view value);
std::string fold_ascii(std::string_view value);

} // namespace cha
