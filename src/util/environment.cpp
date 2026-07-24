#include "util/environment.h"

#include "util/text.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cha {
namespace {

bool is_name(std::string_view value) {
    if (value.empty() || (std::isalpha(static_cast<unsigned char>(value.front())) == 0 && value.front() != '_')) {
        return false;
    }

    for (const char character : value.substr(1)) {
        if (std::isalnum(static_cast<unsigned char>(character)) == 0 && character != '_') {
            return false;
        }
    }
    return true;
}

std::string parse_value(std::string_view value) {
    value = trim_view(value);
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"')
                              || (value.front() == '\'' && value.back() == '\''))) {
        value.remove_prefix(1);
        value.remove_suffix(1);
    }
    return std::string(value);
}

} // namespace

void load_dotenv(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        if (!std::filesystem::exists(path)) {
            return;
        }
        throw std::runtime_error("Failed to read dotenv file '" + path.string() + "'");
    }

    std::string line;
    std::size_t line_number = 0;
    while (std::getline(file, line)) {
        ++line_number;
        const std::string_view entry = trim_view(line);
        if (entry.empty() || entry.front() == '#') {
            continue;
        }

        const std::size_t separator = entry.find('=');
        const std::string_view name = trim_view(entry.substr(0, separator));
        if (separator == std::string_view::npos || !is_name(name)) {
            throw std::runtime_error(
                "Invalid dotenv entry in '" + path.string() + "' at line " + std::to_string(line_number));
        }

        const std::string name_string(name);
        if (std::getenv(name_string.c_str()) == nullptr
            && ::setenv(name_string.c_str(), parse_value(entry.substr(separator + 1)).c_str(), 0) != 0) {
            throw std::runtime_error(
                "Failed to set environment variable '" + name_string + "' from '" + path.string() + "'");
        }
    }
}

} // namespace cha
