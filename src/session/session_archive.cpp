#include "session/session_archive.h"

#include "session/session_delete_conflict.h"
#include "util/logging.h"
#include "util/path_name.h"

#include <cerrno>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <cstdio>
#include <sys/stdio.h>
#else
#include <fcntl.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace cha {
namespace {

[[noreturn]] void report_conflict(const std::filesystem::path& destination) {
    throw SessionDeleteConflictError(
        "A deleted session database already exists at '"
        + utf8_path(destination) + "'");
}

[[noreturn]] void report_failure(
    const char* action,
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    std::error_code error) {
    throw std::filesystem::filesystem_error(action, source, destination, error);
}

// The errors a mount reports when it recognizes the no-replace rename but
// cannot implement it. They select the fallback rather than failing the move.
bool flag_unsupported(int error) {
    return error == EINVAL || error == ENOSYS || error == EOPNOTSUPP;
}

} // namespace

void archive_by_link_without_replacement(
    const std::filesystem::path& source,
    const std::filesystem::path& destination) {
    std::error_code error;
    std::filesystem::create_hard_link(source, destination, error);
    if (error == std::errc::file_exists) report_conflict(destination);
    if (error) {
        report_failure(
            "Failed to link session database into deleted storage",
            source,
            destination,
            error);
    }

    error.clear();
    if (std::filesystem::remove(source, error)) return;
    if (!error) error = std::make_error_code(std::errc::no_such_file_or_directory);

    // Keep a failed operation catalog-visible at its source. The destination
    // was created by this call, so roll it back before reporting the unlink
    // failure. A rollback failure is logged separately because both names now
    // refer to the same intact database and need operator attention.
    std::error_code rollback_error;
    std::filesystem::remove(destination, rollback_error);
    if (rollback_error) {
        log_error("Failed to roll back archived session database link: path="
            + utf8_path(destination));
    }
    report_failure(
        "Failed to remove active session database after archiving",
        source,
        destination,
        error);
}

void archive_without_replacement(
    const std::filesystem::path& source,
    const std::filesystem::path& destination) {
#ifdef _WIN32
    if (::MoveFileExW(source.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
        return;
    }
    const DWORD error = ::GetLastError();
    if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) {
        report_conflict(destination);
    }
    report_failure(
        "Failed to archive session database",
        source,
        destination,
        std::error_code(static_cast<int>(error), std::system_category()));
#else
#ifdef __APPLE__
    const bool renamed =
        ::renamex_np(source.c_str(), destination.c_str(), RENAME_EXCL) == 0;
#else
    const bool renamed = ::syscall(
        SYS_renameat2,
        AT_FDCWD,
        source.c_str(),
        AT_FDCWD,
        destination.c_str(),
        RENAME_NOREPLACE) == 0;
#endif
    if (renamed) return;
    const int error = errno;
    if (error == EEXIST) report_conflict(destination);
    if (flag_unsupported(error)) {
        return archive_by_link_without_replacement(source, destination);
    }
    report_failure(
        "Failed to archive session database",
        source,
        destination,
        std::error_code(error, std::generic_category()));
#endif
}

} // namespace cha
