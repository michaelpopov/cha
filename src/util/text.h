#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace cha {

bool is_space(char character);
std::size_t find_whitespace(std::string_view value);
std::string_view trim_view(std::string_view value);
std::string fold_ascii(std::string_view value);

// Case-insensitive handle matching, shared by the character and persona
// resolvers so both spell "same name" the same way. These compare in place
// rather than folding into a temporary.
bool ascii_iequals(std::string_view left, std::string_view right);
bool starts_with_folded(std::string_view value, std::string_view prefix);
// True when any whitespace-separated word of `name` starts with `handle`.
bool starts_with_name_word(std::string_view name, std::string_view handle);

} // namespace cha
