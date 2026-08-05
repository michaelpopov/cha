#include "session/catalog_lease.h"

#include "util/utf8_path.h"

#include <chrono>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

namespace cha {
class CatalogLease::Impl {
public:
    explicit Impl(const std::filesystem::path& path) {
#ifdef _WIN32
        handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) fail("open or create", path, GetLastError());
#else
        descriptor = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (descriptor == -1) fail("open or create", path, errno);
#endif
    }

    [[nodiscard]] bool try_lock() {
#ifdef _WIN32
        OVERLAPPED region{};
        if (LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                0, MAXDWORD, MAXDWORD, &region)) return true;
        if (GetLastError() == ERROR_LOCK_VIOLATION) return false;
        fail("lock", {}, GetLastError());
#else
        if (flock(descriptor, LOCK_EX | LOCK_NB) == 0) return true;
        if (errno == EWOULDBLOCK) return false;
        fail("lock", {}, errno);
#endif
    }

    ~Impl() {
#ifdef _WIN32
        if (handle != INVALID_HANDLE_VALUE) {
            OVERLAPPED region{};
            (void)UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &region);
            (void)CloseHandle(handle);
        }
#else
        if (descriptor != -1) (void)close(descriptor);
#endif
    }

private:
#ifdef _WIN32
    [[noreturn]] static void fail(const char* action, const std::filesystem::path& path, DWORD error) {
        throw std::system_error(static_cast<int>(error), std::system_category(),
            std::string("Failed to ") + action + " catalog lease '" + utf8_path(path) + "'");
    }
    HANDLE handle{INVALID_HANDLE_VALUE};
#else
    [[noreturn]] static void fail(const char* action, const std::filesystem::path& path, int error) {
        throw std::system_error(error, std::generic_category(),
            std::string("Failed to ") + action + " catalog lease '" + utf8_path(path) + "'");
    }
    int descriptor{-1};
#endif
};

CatalogLease::CatalogLease(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
CatalogLease::~CatalogLease() = default;
CatalogLease::CatalogLease(CatalogLease&&) noexcept = default;
CatalogLease& CatalogLease::operator=(CatalogLease&&) noexcept = default;

CatalogLease CatalogLease::acquire(const std::filesystem::path& directory,
    std::chrono::steady_clock::duration timeout, Now now, Backoff backoff) {
    if (directory.empty()) throw std::invalid_argument("Sessions directory must not be empty");
    if (timeout < std::chrono::steady_clock::duration::zero()) throw std::invalid_argument("Catalog lease timeout must not be negative");
    std::filesystem::create_directories(directory);
    if (!now) now = [] { return std::chrono::steady_clock::now(); };
    if (!backoff) backoff = [] { std::this_thread::sleep_for(std::chrono::milliseconds(10)); };
    const auto deadline = now() + timeout;
    auto impl = std::make_unique<Impl>(lock_path(directory));
    while (!impl->try_lock()) {
        if (now() >= deadline) {
            throw CatalogBusyError("Session catalog is busy");
        }
        backoff();
    }
    return CatalogLease(std::move(impl));
}

std::filesystem::path CatalogLease::lock_path(const std::filesystem::path& directory) {
    if (directory.empty()) throw std::invalid_argument("Sessions directory must not be empty");
    return directory / "catalog.cha-lock";
}

bool CatalogLease::active() const noexcept { return static_cast<bool>(impl_); }
} // namespace cha
