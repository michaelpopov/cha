#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <string_view>

namespace cha {

inline constexpr std::size_t sha256_digest_size = 32;
using Sha256Digest = std::array<unsigned char, sha256_digest_size>;

Sha256Digest sha256_digest(std::string_view contents);
Sha256Digest sha256_file_digest(const std::filesystem::path& path);
Sha256Digest hmac_sha256_digest(
    std::string_view key,
    std::string_view contents);

} // namespace cha
