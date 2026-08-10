#include "util/path_name.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace cha {

std::filesystem::path executable_directory() {
#ifdef _WIN32
    std::vector<wchar_t> buffer(1024);
    while (true) {
        const DWORD length = ::GetModuleFileNameW(
            nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            throw std::runtime_error("Failed to locate the running executable");
        }
        if (length < buffer.size() - 1) {
            return std::filesystem::path(
                       std::wstring(buffer.data(), static_cast<std::size_t>(length)))
                .parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    std::uint32_t size = 0;
    (void)::_NSGetExecutablePath(nullptr, &size);
    if (size == 0) {
        throw std::runtime_error("Failed to locate the running executable");
    }
    std::vector<char> buffer(size);
    if (::_NSGetExecutablePath(buffer.data(), &size) != 0) {
        throw std::runtime_error("Failed to locate the running executable");
    }
    return std::filesystem::weakly_canonical(buffer.data()).parent_path();
#else
    std::vector<char> buffer(1024);
    while (true) {
        const ssize_t length = ::readlink(
            "/proc/self/exe", buffer.data(), buffer.size());
        if (length < 0) {
            throw std::runtime_error(
                "Failed to locate the running executable through /proc/self/exe");
        }
        if (static_cast<std::size_t>(length) < buffer.size()) {
            return std::filesystem::path(
                       std::string(buffer.data(), static_cast<std::size_t>(length)))
                .parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#endif
}

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

void require_path_component(std::string_view name, const std::filesystem::path& source) {
    const std::filesystem::path path = path_from_utf8(name);
    if (name.empty()
        || name.find('/') != std::string_view::npos
        || name.find('\\') != std::string_view::npos
        || path.is_absolute()
        || path.has_parent_path()
        || name == "."
        || name == "..") {
        throw std::runtime_error(
            "Invalid name '" + std::string(name) + "' in '"
            + utf8_path(source) + "'");
    }
}

bool is_url_safe_identifier(std::string_view name) noexcept {
    if (name.empty() || name == "." || name == "..") {
        return false;
    }
    for (const char value : name) {
        const unsigned char character = static_cast<unsigned char>(value);
        const bool ascii_letter =
            (character >= 'A' && character <= 'Z')
            || (character >= 'a' && character <= 'z');
        const bool digit = character >= '0' && character <= '9';
        if (!ascii_letter && !digit
            && character != '-' && character != '.'
            && character != '_' && character != '~') {
            return false;
        }
    }
    return true;
}

void require_url_safe_identifier(
    std::string_view name,
    const std::filesystem::path& source) {
    if (!is_url_safe_identifier(name)) {
        throw std::runtime_error(
            "Invalid URL-safe identifier '" + std::string(name) + "' in '"
            + utf8_path(source) + "'");
    }
}

} // namespace cha
