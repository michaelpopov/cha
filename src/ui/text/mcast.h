#pragma once

#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace cha {

struct MulticastInput {
    std::vector<std::string> handles;
    std::string text;

    bool operator==(const MulticastInput&) const = default;
};

enum class MulticastParseError {
    empty_prompt,
    empty_handle,
    unexpected_comma,
    missing_separator,
};

using MulticastParseResult =
    std::variant<MulticastInput, MulticastParseError>;

[[nodiscard]] MulticastParseResult parse_multicast_input(
    std::string_view argument);
[[nodiscard]] std::string_view multicast_parse_error_message(
    MulticastParseError error);

} // namespace cha

