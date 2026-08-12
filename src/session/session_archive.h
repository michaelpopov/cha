#pragma once

#include <filesystem>

namespace cha {

// Moves one session file to its recoverable-delete destination. The move never
// replaces a file already at the destination: that case is a conflict and
// reports SessionDeleteConflictError with both names left intact.
//
// std::filesystem::rename cannot express this, so the platform's no-replace
// rename flag is used instead. Mounts that reject the flag select the link
// fallback below.
void archive_without_replacement(
    const std::filesystem::path& source,
    const std::filesystem::path& destination);

// The portable same-filesystem fallback for mounts that cannot honor a
// no-replace rename flag, such as the 9p mounts WSL uses for Windows drives.
// Publishing the hard link is atomic and refuses an existing destination; only
// after that succeeds is the source name removed.
//
// It is declared here rather than kept internal because the mounts that select
// it in production are exactly the ones a test host is unlikely to offer, so
// tests would otherwise never reach it.
void archive_by_link_without_replacement(
    const std::filesystem::path& source,
    const std::filesystem::path& destination);

} // namespace cha
