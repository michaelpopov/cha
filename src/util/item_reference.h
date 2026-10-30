#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace cha {

[[nodiscard]] std::optional<std::int64_t> parse_item_reference(std::string_view input);

} // namespace cha
