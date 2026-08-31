#include "util/environment.h"

#include "util/text.h"
#include "util/path_name.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

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

std::vector<DotenvEntry> parse_dotenv(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        if (!std::filesystem::exists(path)) {
            return {};
        }
        throw std::runtime_error(
            "Failed to read dotenv file '" + utf8_path(path) + "'");
    }

    std::vector<DotenvEntry> entries;
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

        entries.push_back({
            .name = std::string(name),
            .value = parse_value(entry.substr(separator + 1)),
        });
    }
    return entries;
}

void apply_dotenv(
    const std::vector<DotenvEntry>& entries,
    const std::filesystem::path& source) {
    for (const DotenvEntry& entry : entries) {
        if (set_environment_variable(entry.name, entry.value, false)) {
            continue;
        }
        std::string message =
            "Failed to set environment variable '" + entry.name + "'";
        if (!source.empty()) {
            message += " from '" + utf8_path(source) + "'";
        }
        throw std::runtime_error(message);
    }
}

void load_dotenv(const std::filesystem::path& path) {
    apply_dotenv(parse_dotenv(path), path);
}

ScopedEnvironmentOverlay::ScopedEnvironmentOverlay(
    const std::vector<DotenvEntry>& entries) {
    try {
        for (const DotenvEntry& entry : entries) {
            if (std::getenv(entry.name.c_str()) != nullptr) continue;
            if (!set_environment_variable(entry.name, entry.value, false)) {
                throw std::runtime_error(
                    "Failed to set environment variable '" + entry.name + "'");
            }
            inserted_.push_back(entry.name);
        }
    } catch (...) {
        restore();
        throw;
    }
}

void ScopedEnvironmentOverlay::restore() noexcept {
    for (const std::string& name : inserted_) {
        (void)unset_environment_variable(name);
    }
    inserted_.clear();
}

ScopedEnvironmentOverlay::~ScopedEnvironmentOverlay() {
    restore();
}

} // namespace cha
