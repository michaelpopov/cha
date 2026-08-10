#include "util/environment.h"

#include "util/text.h"
#include "util/path_name.h"

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

bool set_environment_variable(
    std::string_view name,
    std::string_view value,
    bool overwrite) {
    const std::string name_string(name);
    if (!overwrite && std::getenv(name_string.c_str()) != nullptr) {
        return true;
    }
    const std::string value_string(value);
#ifdef _WIN32
    return ::_putenv_s(name_string.c_str(), value_string.c_str()) == 0;
#else
    return ::setenv(
        name_string.c_str(),
        value_string.c_str(),
        overwrite ? 1 : 0) == 0;
#endif
}

bool unset_environment_variable(std::string_view name) {
    const std::string name_string(name);
#ifdef _WIN32
    return ::_putenv_s(name_string.c_str(), "") == 0;
#else
    return ::unsetenv(name_string.c_str()) == 0;
#endif
}

void load_dotenv(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        if (!std::filesystem::exists(path)) {
            return;
        }
        throw std::runtime_error(
            "Failed to read dotenv file '" + utf8_path(path) + "'");
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
                "Invalid dotenv entry in '" + utf8_path(path)
                + "' at line " + std::to_string(line_number));
        }

        const std::string name_string(name);
        const std::string value =
            parse_value(entry.substr(separator + 1));
        if (!set_environment_variable(name_string, value, false)) {
            throw std::runtime_error(
                "Failed to set environment variable '" + name_string
                + "' from '" + utf8_path(path) + "'");
        }
    }
}

} // namespace cha
