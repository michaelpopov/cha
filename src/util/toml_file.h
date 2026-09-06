#pragma once

#include <toml++/toml.hpp>

#include <filesystem>
#include <string_view>
#include <utility>

namespace cha {

toml::table read_toml_file(
    const std::filesystem::path& path, std::string_view kind);

void write_toml_file(
    const std::filesystem::path& path, const toml::table& table);

template<typename Mutate>
void rewrite_toml_file(const std::filesystem::path& path, Mutate mutate) {
    toml::table table = read_toml_file(path, "config file");
    mutate(table);
    write_toml_file(path, table);
}

} // namespace cha
