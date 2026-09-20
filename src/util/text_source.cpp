#include "util/text_source.h"

#include "util/path_name.h"

#include <fstream>
#include <iterator>
#include <set>

namespace cha {

TextSource::TextSource(std::filesystem::path root, const TextFiles& files)
    : root_(std::filesystem::absolute(root).lexically_normal()), files_(&files) {}

std::string TextSource::name(const std::filesystem::path& path) const {
    return generic_utf8_path(
        std::filesystem::absolute(path).lexically_normal().lexically_relative(root_));
}

std::optional<std::string> TextSource::read(const std::filesystem::path& path) const {
    if (files_) {
        const auto found = files_->find(name(path));
        if (found == files_->end()) return std::nullopt;
        return found->second;
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::string content{std::istreambuf_iterator<char>(input), {}};
    if (input.bad()) return std::nullopt;
    return content;
}

std::filesystem::path TextSource::canonical(
    const std::filesystem::path& path, std::error_code& error) const {
    if (!files_) return std::filesystem::weakly_canonical(path, error);
    error.clear();
    return std::filesystem::absolute(path).lexically_normal();
}

std::filesystem::file_status TextSource::status(
    const std::filesystem::path& path, std::error_code& error) const {
    if (!files_) return std::filesystem::status(path, error);
    error.clear();
    const std::string key = name(path);
    using Type = std::filesystem::file_type;
    if (files_->contains(key)) return std::filesystem::file_status(Type::regular);
    if (key == ".") return std::filesystem::file_status(Type::directory);
    const std::string prefix = key + "/";
    const auto first = files_->lower_bound(prefix);
    return std::filesystem::file_status(
        first != files_->end() && first->first.starts_with(prefix)
            ? Type::directory : Type::not_found);
}

bool TextSource::exists(const std::filesystem::path& path) const {
    if (!files_) return std::filesystem::exists(path);
    std::error_code error;
    return std::filesystem::exists(status(path, error));
}

bool TextSource::is_directory(const std::filesystem::path& path) const {
    if (!files_) return std::filesystem::is_directory(path);
    std::error_code error;
    return std::filesystem::is_directory(status(path, error));
}

bool TextSource::is_regular_file(const std::filesystem::path& path) const {
    if (!files_) return std::filesystem::is_regular_file(path);
    std::error_code error;
    return std::filesystem::is_regular_file(status(path, error));
}

bool TextSource::is_symlink(const std::filesystem::path& path) const {
    return !files_ && std::filesystem::is_symlink(path);
}

std::vector<std::filesystem::path> TextSource::entries(
    const std::filesystem::path& directory, bool recursive) const {
    std::set<std::filesystem::path> result;
    if (files_) {
        const std::string key = name(directory);
        const std::string prefix = key == "." ? "" : key + "/";
        for (auto it = files_->lower_bound(prefix);
             it != files_->end() && it->first.starts_with(prefix); ++it) {
            const std::string_view tail = std::string_view(it->first).substr(prefix.size());
            std::size_t end = tail.find('/');
            for (;;) {
                result.insert(directory / path_from_utf8(tail.substr(0, end)));
                if (!recursive || end == std::string_view::npos) break;
                end = tail.find('/', end + 1);
            }
        }
    } else if (recursive) {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(directory)) {
            result.insert(entry.path());
        }
    } else {
        for (const auto& entry : std::filesystem::directory_iterator(directory)) {
            result.insert(entry.path());
        }
    }
    return {result.begin(), result.end()};
}

} // namespace cha
