#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace cha {

// Returns the UTF-8 spelling of a filesystem path for APIs, diagnostics, and
// persisted text. std::filesystem keeps native UTF-16 paths on Windows.
std::string utf8_path(const std::filesystem::path& path);

// Constructs a native filesystem path from UTF-8 application text.
std::filesystem::path path_from_utf8(std::string_view value);

#ifdef _WIN32
// Converts Windows UTF-16 command-line text to UTF-8 application text.
std::string utf8_from_wide(std::wstring_view value);
#endif

} // namespace cha
