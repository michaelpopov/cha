#pragma once

#include <filesystem>
#include <string_view>

namespace cha {

void require_path_component(std::string_view name, const std::filesystem::path& source);

} // namespace cha
