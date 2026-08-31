#include "util/private_filesystem.h"

#include "util/path_name.h"

#ifdef _WIN32
#include <windows.h>
#include <accctrl.h>
#include <aclapi.h>
#include <sddl.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace cha {
namespace {

[[noreturn]] void fail_path(
    const char* action,
    const std::filesystem::path& path) {
    throw std::runtime_error(
        std::string("Failed to ") + action + " '" + utf8_path(path) + "'");
}

std::filesystem::file_status inspected_status(
    const std::filesystem::path& path) {
    std::error_code error;
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(path, error);
    if (status.type() == std::filesystem::file_type::not_found) {
        return status;
    }
    if (error) {
        throw std::runtime_error(
            "Failed to inspect '" + utf8_path(path) + "': " + error.message());
    }
    return status;
}

void require_absent_or_regular_file(const std::filesystem::path& path) {
    const std::filesystem::file_status status = inspected_status(path);
    if (!std::filesystem::exists(status)) return;
    if (!std::filesystem::is_regular_file(status)) {
        throw std::runtime_error(
            "Path '" + utf8_path(path) + "' is not a regular file");
    }
}

#ifdef _WIN32

[[noreturn]] void fail_windows(
    const char* action,
    const std::filesystem::path& path) {
    throw std::system_error(
        static_cast<int>(::GetLastError()),
        std::system_category(),
        std::string("Failed to ") + action + " '" + utf8_path(path) + "'");
}

class CurrentUserPrivateSecurity {
public:
    CurrentUserPrivateSecurity() {
        HANDLE token = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
            fail_windows("read the current user", {});
        }
        DWORD size = 0;
        ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<unsigned char> buffer(size);
        const BOOL ok = ::GetTokenInformation(
            token, TokenUser, buffer.data(), size, &size);
        const DWORD token_error = ::GetLastError();
        ::CloseHandle(token);
        if (!ok) {
            ::SetLastError(token_error);
            fail_windows("read the current user", {});
        }

        const auto* user = reinterpret_cast<TOKEN_USER*>(buffer.data());
        LPWSTR sid_string = nullptr;
        if (!::ConvertSidToStringSidW(user->User.Sid, &sid_string)) {
            fail_windows("read the current user", {});
        }
        const std::wstring sddl = std::wstring(L"O:") + sid_string
            + L"D:P(A;;FA;;;" + sid_string + L")(A;;FA;;;SY)";
        ::LocalFree(sid_string);

        ULONG sd_size = 0;
        if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl.c_str(), SDDL_REVISION_1, &sd_, &sd_size)) {
            fail_windows("build a private security descriptor", {});
        }
        attributes_.nLength = sizeof(attributes_);
        attributes_.lpSecurityDescriptor = sd_;
        attributes_.bInheritHandle = FALSE;

        const DWORD sid_length = ::GetLengthSid(user->User.Sid);
        user_sid_.resize(sid_length);
        if (!::CopySid(sid_length, user_sid_.data(), user->User.Sid)) {
            fail_windows("read the current user", {});
        }
    }

    CurrentUserPrivateSecurity(const CurrentUserPrivateSecurity&) = delete;
    CurrentUserPrivateSecurity& operator=(
        const CurrentUserPrivateSecurity&) = delete;

    ~CurrentUserPrivateSecurity() {
        if (sd_ != nullptr) ::LocalFree(sd_);
    }

    SECURITY_ATTRIBUTES* attributes() noexcept { return &attributes_; }

    void apply(const std::filesystem::path& path) const {
        BOOL present = FALSE;
        BOOL defaulted = FALSE;
        PACL dacl = nullptr;
        if (!::GetSecurityDescriptorDacl(sd_, &present, &dacl, &defaulted)
            || !present
            || dacl == nullptr) {
            fail_path("establish owner-only permissions on", path);
        }
        const DWORD error = ::SetNamedSecurityInfoW(
            const_cast<wchar_t*>(path.c_str()),
            SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION
                | PROTECTED_DACL_SECURITY_INFORMATION,
            user_sid(),
            nullptr,
            dacl,
            nullptr);
        if (error != ERROR_SUCCESS) {
            ::SetLastError(error);
            fail_windows("establish owner-only permissions on", path);
        }
    }

    void verify(const std::filesystem::path& path) const {
        PSID owner = nullptr;
        PACL dacl = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        const DWORD error = ::GetNamedSecurityInfoW(
            const_cast<wchar_t*>(path.c_str()),
            SE_FILE_OBJECT,
            OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
            &owner,
            nullptr,
            &dacl,
            nullptr,
            &descriptor);
        if (error != ERROR_SUCCESS) {
            ::SetLastError(error);
            fail_windows("verify owner-only permissions on", path);
        }
        struct LocalFreeGuard {
            PSECURITY_DESCRIPTOR value{};
            ~LocalFreeGuard() {
                if (value != nullptr) ::LocalFree(value);
            }
        } guard{descriptor};

        if (dacl == nullptr || owner == nullptr
            || !::EqualSid(owner, user_sid())) {
            fail_path("establish owner-only permissions on", path);
        }
        SECURITY_DESCRIPTOR_CONTROL control{};
        DWORD revision{};
        if (!::GetSecurityDescriptorControl(descriptor, &control, &revision)
            || (control & SE_DACL_PROTECTED) == 0) {
            fail_path("establish owner-only permissions on", path);
        }

        SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;
        PSID system = nullptr;
        if (!::AllocateAndInitializeSid(
                &nt_authority,
                1,
                SECURITY_LOCAL_SYSTEM_RID,
                0,
                0,
                0,
                0,
                0,
                0,
                0,
                &system)) {
            fail_windows("verify owner-only permissions on", path);
        }
        struct SidGuard {
            PSID value{};
            ~SidGuard() {
                if (value != nullptr) ::FreeSid(value);
            }
        } system_guard{system};

        ACL_SIZE_INFORMATION info{};
        if (!::GetAclInformation(
                dacl, &info, sizeof(info), AclSizeInformation)) {
            fail_windows("verify owner-only permissions on", path);
        }
        for (DWORD index = 0; index < info.AceCount; ++index) {
            LPVOID ace = nullptr;
            if (!::GetAce(dacl, index, &ace)) {
                fail_windows("verify owner-only permissions on", path);
            }
            const auto* header = static_cast<const ACE_HEADER*>(ace);
            if (header->AceType != ACCESS_ALLOWED_ACE_TYPE) {
                fail_path("establish owner-only permissions on", path);
            }
            auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
            const PSID sid = reinterpret_cast<PSID>(&allowed->SidStart);
            if (!::EqualSid(sid, user_sid()) && !::EqualSid(sid, system)) {
                fail_path("establish owner-only permissions on", path);
            }
        }
    }

private:
    [[nodiscard]] PSID user_sid() const noexcept {
        return reinterpret_cast<PSID>(
            const_cast<unsigned char*>(user_sid_.data()));
    }

    PSECURITY_DESCRIPTOR sd_{};
    SECURITY_ATTRIBUTES attributes_{};
    std::vector<unsigned char> user_sid_;
};

void write_all(HANDLE handle, std::string_view contents) {
    while (!contents.empty()) {
        DWORD written = 0;
        const DWORD chunk = contents.size() > MAXDWORD
            ? MAXDWORD
            : static_cast<DWORD>(contents.size());
        if (!::WriteFile(handle, contents.data(), chunk, &written, nullptr)
            || written == 0) {
            throw std::runtime_error("Failed to write a private file");
        }
        contents.remove_prefix(written);
    }
}

#else

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

[[noreturn]] void fail_posix(
    const char* action,
    const std::filesystem::path& path) {
    throw std::system_error(
        errno,
        std::generic_category(),
        std::string("Failed to ") + action + " '" + utf8_path(path) + "'");
}

void verify_posix_mode(
    const std::filesystem::path& path,
    mode_t type,
    mode_t mode) {
    struct stat info {};
    if (::lstat(path.c_str(), &info) != 0) fail_posix("inspect", path);
    if ((info.st_mode & S_IFMT) != type) {
        throw std::runtime_error(
            "Path '" + utf8_path(path) + "' has an unexpected type");
    }
    if ((info.st_mode & 0777) != mode) {
        fail_path("establish owner-only permissions on", path);
    }
}

void write_all(int fd, std::string_view contents) {
    while (!contents.empty()) {
        const ssize_t written =
            ::write(fd, contents.data(), contents.size());
        if (written < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("Failed to write a private file");
        }
        contents.remove_prefix(static_cast<std::size_t>(written));
    }
}

#endif

std::filesystem::path unique_temporary_path(
    const std::filesystem::path& destination,
    std::mt19937_64& random) {
    std::filesystem::path candidate = destination;
    candidate += ".tmp-";
    candidate += std::to_string(random());
    return candidate;
}

} // namespace

void require_regular_file(const std::filesystem::path& path) {
    if (!std::filesystem::is_regular_file(inspected_status(path))) {
        throw std::runtime_error(
            "Path '" + utf8_path(path) + "' is not a regular file");
    }
}

void require_directory(const std::filesystem::path& path) {
    if (!std::filesystem::is_directory(inspected_status(path))) {
        throw std::runtime_error(
            "Path '" + utf8_path(path) + "' is not a directory");
    }
}

void create_private_directory(const std::filesystem::path& path) {
#ifdef _WIN32
    CurrentUserPrivateSecurity security;
    if (!::CreateDirectoryW(path.c_str(), security.attributes())) {
        fail_windows("create private directory", path);
    }
    try {
        security.verify(path);
    } catch (...) {
        ::RemoveDirectoryW(path.c_str());
        throw;
    }
#else
    if (::mkdir(path.c_str(), 0700) != 0) fail_posix("create private directory", path);
    try {
        verify_posix_mode(path, S_IFDIR, 0700);
    } catch (...) {
        ::rmdir(path.c_str());
        throw;
    }
#endif
}

void create_private_file(
    const std::filesystem::path& path,
    std::string_view contents) {
    require_absent_or_regular_file(path);

    std::mt19937_64 random(std::random_device{}());
#ifdef _WIN32
    CurrentUserPrivateSecurity security;
    HANDLE handle = INVALID_HANDLE_VALUE;
    std::filesystem::path temporary;
    for (std::size_t attempt{}; attempt != 100; ++attempt) {
        temporary = unique_temporary_path(path, random);
        handle = ::CreateFileW(
            temporary.c_str(),
            GENERIC_WRITE,
            0,
            security.attributes(),
            CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
        if (handle != INVALID_HANDLE_VALUE) break;
        if (::GetLastError() != ERROR_FILE_EXISTS
            && ::GetLastError() != ERROR_ALREADY_EXISTS) {
            fail_windows("create private file", path);
        }
    }
    if (handle == INVALID_HANDLE_VALUE) {
        fail_path("create private file", path);
    }
    bool keep_temporary = false;
    try {
        write_all(handle, contents);
        if (!::FlushFileBuffers(handle)) {
            fail_windows("write private file", path);
        }
        ::CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
        require_absent_or_regular_file(path);
        if (!::MoveFileExW(
                temporary.c_str(),
                path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            fail_windows("create private file", path);
        }
        keep_temporary = true;
        security.verify(path);
    } catch (...) {
        if (handle != INVALID_HANDLE_VALUE) ::CloseHandle(handle);
        if (!keep_temporary) ::DeleteFileW(temporary.c_str());
        throw;
    }
#else
    int fd = -1;
    std::filesystem::path temporary;
    for (std::size_t attempt{}; attempt != 100; ++attempt) {
        temporary = unique_temporary_path(path, random);
        fd = ::open(
            temporary.c_str(),
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
            0600);
        if (fd >= 0) break;
        if (errno != EEXIST) fail_posix("create private file", path);
    }
    if (fd < 0) fail_path("create private file", path);
    bool keep_temporary = false;
    try {
        write_all(fd, contents);
        if (::fsync(fd) != 0) fail_posix("write private file", path);
        if (::close(fd) != 0) fail_posix("write private file", path);
        fd = -1;
        require_absent_or_regular_file(path);
        if (::rename(temporary.c_str(), path.c_str()) != 0) {
            fail_posix("create private file", path);
        }
        keep_temporary = true;
        verify_posix_mode(path, S_IFREG, 0600);
    } catch (...) {
        if (fd >= 0) ::close(fd);
        if (!keep_temporary) ::unlink(temporary.c_str());
        throw;
    }
#endif
}

void tighten_private_file(const std::filesystem::path& path) {
    require_regular_file(path);
#ifdef _WIN32
    CurrentUserPrivateSecurity security;
    security.apply(path);
    security.verify(path);
#else
    const int fd = ::open(
        path.c_str(),
        O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) fail_posix("establish owner-only permissions on", path);
    struct stat info {};
    if (::fstat(fd, &info) != 0) {
        const int error = errno;
        ::close(fd);
        errno = error;
        fail_posix("establish owner-only permissions on", path);
    }
    if (!S_ISREG(info.st_mode)) {
        ::close(fd);
        throw std::runtime_error(
            "Path '" + utf8_path(path) + "' is not a regular file");
    }
    if (::fchmod(fd, 0600) != 0) {
        const int error = errno;
        ::close(fd);
        errno = error;
        fail_posix("establish owner-only permissions on", path);
    }
    if (::fstat(fd, &info) != 0) {
        const int error = errno;
        ::close(fd);
        errno = error;
        fail_posix("establish owner-only permissions on", path);
    }
    ::close(fd);
    if ((info.st_mode & 0777) != 0600) {
        fail_path("establish owner-only permissions on", path);
    }
#endif
}

} // namespace cha
