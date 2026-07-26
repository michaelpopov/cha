#include "util/utf8_path.h"

#ifdef _WIN32
#include <windows.h>
#endif

#include <stdexcept>

namespace cha {

std::string utf8_path(const std::filesystem::path& path) {
    const std::u8string value = path.u8string();
    return std::string(
        reinterpret_cast<const char*>(value.data()),
        value.size());
}

std::filesystem::path path_from_utf8(std::string_view value) {
    std::u8string encoded;
    encoded.reserve(value.size());
    for (const char byte : value) {
        encoded.push_back(
            static_cast<char8_t>(static_cast<unsigned char>(byte)));
    }
    return std::filesystem::path(encoded);
}

#ifdef _WIN32
std::string utf8_from_wide(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int required = ::WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (required == 0) {
        throw std::runtime_error("Failed to convert Windows text to UTF-8");
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    if (::WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            required,
            nullptr,
            nullptr) == 0) {
        throw std::runtime_error("Failed to convert Windows text to UTF-8");
    }
    return result;
}
#endif

} // namespace cha
