#include "util/path_name.h"

#include "util/utf8_path.h"

#include <stdexcept>
#include <string>

namespace cha {

void require_path_component(std::string_view name, const std::filesystem::path& source) {
    const std::filesystem::path path = path_from_utf8(name);
    if (name.empty()
        || name.find('/') != std::string_view::npos
        || name.find('\\') != std::string_view::npos
        || path.is_absolute()
        || path.has_parent_path()
        || name == "."
        || name == "..") {
        throw std::runtime_error(
            "Invalid name '" + std::string(name) + "' in '"
            + utf8_path(source) + "'");
    }
}

bool is_url_safe_identifier(std::string_view name) noexcept {
    if (name.empty() || name == "." || name == "..") {
        return false;
    }
    for (const char value : name) {
        const unsigned char character = static_cast<unsigned char>(value);
        const bool ascii_letter =
            (character >= 'A' && character <= 'Z')
            || (character >= 'a' && character <= 'z');
        const bool digit = character >= '0' && character <= '9';
        if (!ascii_letter && !digit
            && character != '-' && character != '.'
            && character != '_' && character != '~') {
            return false;
        }
    }
    return true;
}

void require_url_safe_identifier(
    std::string_view name,
    const std::filesystem::path& source) {
    if (!is_url_safe_identifier(name)) {
        throw std::runtime_error(
            "Invalid URL-safe identifier '" + std::string(name) + "' in '"
            + utf8_path(source) + "'");
    }
}

} // namespace cha
