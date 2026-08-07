#include "util/executable_path.h"

#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <cerrno>
#include <unistd.h>
#endif

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

} // namespace cha
