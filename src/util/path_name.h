#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace cha {

// Returns the directory containing the running executable without relying on
// argv[0] or the process working directory.
std::filesystem::path executable_directory();

// Returns the UTF-8 spelling of a filesystem path for APIs, diagnostics, and
// persisted text. std::filesystem keeps native UTF-16 paths on Windows.
std::string utf8_path(const std::filesystem::path& path);

// Constructs a native filesystem path from UTF-8 application text.
std::filesystem::path path_from_utf8(std::string_view value);

#ifdef _WIN32
// Converts Windows UTF-16 command-line text to UTF-8 application text.
std::string utf8_from_wide(std::wstring_view value);
#endif

void require_path_component(std::string_view name, const std::filesystem::path& source);

// Stable identifiers embedded in web route segments use only RFC 3986
// unreserved ASCII characters. Display names and labels are not identifiers
// and intentionally do not use this restriction.
[[nodiscard]] bool is_url_safe_identifier(std::string_view name) noexcept;
void require_url_safe_identifier(
    std::string_view name,
    const std::filesystem::path& source);

} // namespace cha
