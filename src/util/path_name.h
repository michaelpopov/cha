#pragma once

#include <filesystem>
#include <string_view>

namespace cha {

void require_path_component(std::string_view name, const std::filesystem::path& source);

// Stable identifiers embedded in web route segments use only RFC 3986
// unreserved ASCII characters. Display names and labels are not identifiers
// and intentionally do not use this restriction.
[[nodiscard]] bool is_url_safe_identifier(std::string_view name) noexcept;
void require_url_safe_identifier(
    std::string_view name,
    const std::filesystem::path& source);

} // namespace cha
