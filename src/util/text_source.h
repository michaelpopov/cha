#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace cha {

using TextFiles = std::map<std::string, std::string, std::less<>>;

// Reads configuration and prompt text either from disk or from SQLite rows.
// Stored names are relative to root; the in-memory mode never consults disk.
class TextSource {
public:
    TextSource() = default;
    TextSource(std::filesystem::path root, const TextFiles& files);

    [[nodiscard]] bool in_memory() const { return files_ != nullptr; }
    [[nodiscard]] std::string name(const std::filesystem::path& path) const;
    [[nodiscard]] std::optional<std::string> read(const std::filesystem::path& path) const;
    [[nodiscard]] std::filesystem::path canonical(
        const std::filesystem::path& path, std::error_code& error) const;
    [[nodiscard]] std::filesystem::file_status status(
        const std::filesystem::path& path, std::error_code& error) const;
    [[nodiscard]] bool exists(const std::filesystem::path& path) const;
    [[nodiscard]] bool is_directory(const std::filesystem::path& path) const;
    [[nodiscard]] bool is_regular_file(const std::filesystem::path& path) const;
    [[nodiscard]] bool is_symlink(const std::filesystem::path& path) const;
    [[nodiscard]] std::vector<std::filesystem::path> entries(
        const std::filesystem::path& directory, bool recursive = false) const;

private:
    std::filesystem::path root_;
    const TextFiles* files_{};
};

} // namespace cha
