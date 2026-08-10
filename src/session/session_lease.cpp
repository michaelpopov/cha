#include "session/session_lease.h"

#include "util/path_name.h"

#include <system_error>
#include <string>
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
namespace {

std::string busy_message(const std::filesystem::path& database_path) {
    return "Session already in use: '"
        + utf8_path(database_path.stem()) + "'";
}

} // namespace

class SessionLease::Impl {
public:
    Impl(
        const std::filesystem::path& path,
        const std::filesystem::path& database_path) {
#ifdef _WIN32
        handle = CreateFileW(
            path.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            fail("open or create", path, GetLastError());
        }

        OVERLAPPED lock_region{};
        if (!LockFileEx(
                handle,
                LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                0,
                MAXDWORD,
                MAXDWORD,
                &lock_region)) {
            const DWORD error = GetLastError();
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
            if (error == ERROR_LOCK_VIOLATION) {
                throw SessionBusyError(busy_message(database_path));
            }
            fail("lock", path, error);
        }
#else
        descriptor = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (descriptor == -1) {
            fail("open or create", path, errno);
        }

        // Session databases are restricted to local filesystems. flock locks
        // the open file description, so another descriptor in this process
        // cannot acquire or inadvertently release this lease. On NFS, flock
        // may be implemented with process-owned fcntl locks instead.
        if (flock(descriptor, LOCK_EX | LOCK_NB) == -1) {
            const int error = errno;
            close(descriptor);
            descriptor = -1;
            if (error == EWOULDBLOCK) {
                throw SessionBusyError(busy_message(database_path));
            }
            fail("lock", path, error);
        }
#endif
    }

    ~Impl() {
#ifdef _WIN32
        if (handle != INVALID_HANDLE_VALUE) {
            OVERLAPPED lock_region{};
            (void)UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &lock_region);
            (void)CloseHandle(handle);
        }
#else
        if (descriptor != -1) {
            (void)close(descriptor);
        }
#endif
    }

private:
#ifdef _WIN32
    [[noreturn]] static void fail(
        const char* action,
        const std::filesystem::path& path,
        DWORD error) {
        throw std::system_error(
            static_cast<int>(error),
            std::system_category(),
            std::string("Failed to ") + action + " session lease '"
                + utf8_path(path) + "'");
    }

    HANDLE handle{INVALID_HANDLE_VALUE};
#else
    [[noreturn]] static void fail(
        const char* action,
        const std::filesystem::path& path,
        int error) {
        throw std::system_error(
            error,
            std::generic_category(),
            std::string("Failed to ") + action + " session lease '"
                + utf8_path(path) + "'");
    }

    int descriptor{-1};
#endif
};

SessionLease::SessionLease(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {
}

SessionLease::~SessionLease() = default;
SessionLease::SessionLease(SessionLease&& other) noexcept = default;
SessionLease& SessionLease::operator=(SessionLease&& other) noexcept = default;

SessionLease SessionLease::acquire(const std::filesystem::path& database_path) {
    const std::filesystem::path path = companion_path(database_path);
    return SessionLease(std::make_unique<Impl>(path, database_path));
}

SessionLease SessionLease::inactive_for_testing() {
    return SessionLease(nullptr);
}

std::filesystem::path SessionLease::companion_path(
    const std::filesystem::path& database_path) {
    if (database_path.empty() || database_path.filename().empty()) {
        throw std::invalid_argument("Session database path must name a file");
    }
    std::filesystem::path result = database_path;
    result += ".cha-lock";
    return result;
}

bool SessionLease::active() const noexcept {
    return static_cast<bool>(impl_);
}

} // namespace cha
