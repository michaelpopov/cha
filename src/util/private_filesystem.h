#pragma once

#include <filesystem>
#include <string_view>

namespace cha {

// Creates `path` as a new owner-only directory. Fails if the path already
// exists, including as a symbolic link.
void create_private_directory(const std::filesystem::path& path);

// Creates or replaces `path` as an owner-only regular file. Rejects symbolic
// links and unexpected file types.
void create_private_file(
    const std::filesystem::path& path,
    std::string_view contents);

// Tightens an existing regular file to owner-only access and verifies the
// result. Rejects symbolic links and unexpected file types.
void tighten_private_file(const std::filesystem::path& path);

void require_regular_file(const std::filesystem::path& path);
void require_directory(const std::filesystem::path& path);

} // namespace cha
