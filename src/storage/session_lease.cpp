#include "storage/session_lease.h"

#include "util/path_name.h"
#include "util/crypto.h"

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

class SessionLease::Impl {
public:
    Impl(
        const std::filesystem::path& path,
        std::string busy_message, Mode mode)
        : path_(path), busy_message_(std::move(busy_message)), mode_(mode) {
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

        try { lock(mode); } catch (...) {
            CloseHandle(handle);
            handle = INVALID_HANDLE_VALUE;
            throw;
        }
#else
        descriptor = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (descriptor == -1) {
            fail("open or create", path, errno);
        }

        // Workspace databases are restricted to local filesystems. flock locks
        // the open file description, so another descriptor in this process
        // cannot acquire or inadvertently release this lease. On NFS, flock
        // may be implemented with process-owned fcntl locks instead.
        try { lock(mode); } catch (...) {
            close(descriptor);
            descriptor = -1;
            throw;
        }
#endif
    }

    void lock(Mode mode) {
#ifdef _WIN32
        OVERLAPPED region{};
        const DWORD flags = LOCKFILE_FAIL_IMMEDIATELY
            | (mode == Mode::exclusive ? LOCKFILE_EXCLUSIVE_LOCK : 0);
        if (!LockFileEx(handle, flags, 0, MAXDWORD, MAXDWORD, &region)) {
            const DWORD error = GetLastError();
            if (error == ERROR_LOCK_VIOLATION) throw SessionBusyError(busy_message_);
            fail("lock", path_, error);
        }
#else
        if (flock(descriptor, (mode == Mode::exclusive ? LOCK_EX : LOCK_SH) | LOCK_NB) == -1) {
            const int error = errno;
            if (error == EWOULDBLOCK || error == EAGAIN) throw SessionBusyError(busy_message_);
            fail("lock", path_, error);
        }
#endif
        mode_ = mode;
        locked = true;
    }

    void change_mode(Mode mode) {
        if (locked && mode == mode_) return;
        if (locked) {
#ifdef _WIN32
            OVERLAPPED region{};
            if (!UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &region)) {
                fail("unlock", path_, GetLastError());
            }
#else
            if (flock(descriptor, LOCK_UN) == -1) fail("unlock", path_, errno);
#endif
            locked = false;
        }
        lock(mode);
    }

    bool locked{};
    std::filesystem::path path_;
    std::string busy_message_;
    Mode mode_;

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
            std::string("Failed to ") + action + " database lease '"
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
            std::string("Failed to ") + action + " database lease '"
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

SessionLease SessionLease::acquire(
    const std::filesystem::path& database_path,
    std::string busy_message, Mode mode) {
    return acquire_file(companion_path(database_path), std::move(busy_message), mode);
}

SessionLease SessionLease::acquire_file(
    const std::filesystem::path& path, std::string busy_message, Mode mode) {
    return SessionLease(std::make_unique<Impl>(path, std::move(busy_message), mode));
}

SessionLease SessionLease::acquire_main(const std::filesystem::path& database_path) {
    auto path = std::filesystem::weakly_canonical(database_path);
    path += ".cha-main-lock";
    return acquire_file(path, vault_in_use_message);
}

std::filesystem::path SessionLease::session_path(
    const std::filesystem::path& database_path,
    std::string_view forum_id, std::string_view session_id) {
    auto directory = std::filesystem::weakly_canonical(database_path);
    directory += ".cha-sessions";
    std::filesystem::create_directories(directory);
    std::string identity(forum_id);
    identity.push_back('\0');
    identity.append(session_id);
    const auto digest = sha256_digest(identity);
    constexpr char hex[] = "0123456789abcdef";
    std::string name;
    for (unsigned char byte : digest) {
        name.push_back(hex[byte >> 4]);
        name.push_back(hex[byte & 15]);
    }
    return directory / (name + ".lock");
}

std::filesystem::path SessionLease::companion_path(
    const std::filesystem::path& database_path) {
    if (database_path.empty() || database_path.filename().empty()) {
        throw std::invalid_argument("Database path must name a file");
    }
    std::filesystem::path result = std::filesystem::weakly_canonical(database_path);
    result += ".cha-lock";
    return result;
}

bool SessionLease::active() const noexcept {
    return impl_ && impl_->locked;
}

void SessionLease::change_mode(Mode mode) { impl_->change_mode(mode); }
SessionLease::Mode SessionLease::mode() const noexcept { return impl_->mode_; }

} // namespace cha
