#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace cha {

inline constexpr std::size_t text_file_byte_limit = 256 * 1024;

// A missing last path component with spaces can be an ordinary prompt.
// Reads at most one byte past the limit; call validate_text_file on the result.
[[nodiscard]] std::optional<std::string> read_text_file(const std::filesystem::path& path);
void validate_text_file(std::string_view text, std::string_view too_large_message);

} // namespace cha
