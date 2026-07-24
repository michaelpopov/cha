#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace cha {

[[nodiscard]] bool is_space(char character);
[[nodiscard]] std::size_t find_whitespace(std::string_view value);
[[nodiscard]] std::string_view trim_view(std::string_view value);
[[nodiscard]] std::string fold_ascii(std::string_view value);

} // namespace cha
