#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace cha {

// Validates an authored public entity name without modifying its spelling.
void validate_public_name(
    std::string_view name,
    std::string_view entity,
    const std::filesystem::path& source,
    bool participant_name = false);

// Validates the optional single-line descriptive metadata used by inventory.
void validate_description(
    std::string_view description,
    std::string_view entity,
    const std::filesystem::path& source);

} // namespace cha
