#include "util/toml_file.h"

#include "util/path_name.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace cha {
namespace {

std::filesystem::path temporary_toml_path(const std::filesystem::path& path) {
    static std::atomic_uint64_t sequence{};
    std::filesystem::path temporary = path;
    temporary += ".temp."
        + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count())
        + "." + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
    return temporary;
}

} // namespace

toml::table read_toml_file(
    const std::filesystem::path& path, std::string_view kind) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            "Failed to read " + std::string(kind) + " '" + utf8_path(path) + "'");
    }
    return toml::parse(input, utf8_path(path));
}

void write_toml_file(
    const std::filesystem::path& path, const toml::table& table) {
    const std::filesystem::path temporary = temporary_toml_path(path);
    try {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << table << '\n';
        output.flush();
        if (!output) {
            throw std::runtime_error(
                "Failed to write temporary config '" + utf8_path(temporary) + "'");
        }
        output.close();
        if (!output) {
            throw std::runtime_error(
                "Failed to close temporary config '" + utf8_path(temporary) + "'");
        }
        std::filesystem::rename(temporary, path);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

} // namespace cha
