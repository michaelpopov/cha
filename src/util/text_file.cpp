#include "util/text_file.h"

#include "util/path_name.h"
#include "util/text.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace cha {

void validate_text_file(std::string_view text, std::string_view too_large_message, bool allow_empty) {
    if (text.size() > text_file_byte_limit) throw std::invalid_argument(std::string(too_large_message));
    if (!allow_empty && trim_view(text).empty()) throw std::invalid_argument("The file is empty.");
    if (std::ranges::any_of(text, [](unsigned char c) {
            return c < 32 && c != '\n' && c != '\r' && c != '\t' && c != '\f';
        })) {
        throw std::invalid_argument("Choose a UTF-8 text file.");
    }
    if (!is_valid_utf8(text)) throw std::invalid_argument("Choose a UTF-8 text file.");
}

bool is_valid_utf8(std::string_view text) {
    try {
        (void)nlohmann::json(text).dump();
        return true;
    } catch (const nlohmann::json::type_error&) {
        return false;
    }
}

std::optional<std::string> read_text_file(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::status(path, error);
    const bool missing = status.type() == std::filesystem::file_type::not_found;
    if (missing || error) {
        if (std::ranges::any_of(utf8_path(path.filename()), is_space)) return std::nullopt;
        if (missing) throw std::invalid_argument("That file was not found.");
        throw std::invalid_argument("The file could not be read. Check the path and permissions.");
    }
    if (!std::filesystem::is_regular_file(status)) {
        throw std::invalid_argument("The path does not refer to a regular file.");
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::invalid_argument("The file could not be read. Check the path and permissions.");
    // Read one extra byte to reject large or growing files without loading them all.
    std::string text(text_file_byte_limit + 1, '\0');
    file.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(file.gcount()));
    if (file.bad() || (file.fail() && !file.eof())) {
        throw std::invalid_argument("The file could not be read. Try again.");
    }
    if (text.size() <= text_file_byte_limit && text.starts_with("\xef\xbb\xbf")) text.erase(0, 3);
    return text;
}

} // namespace cha
