#pragma once

#include <filesystem>

namespace cha {

// Loads variables from a dotenv file without replacing values inherited by the process.
void load_dotenv(const std::filesystem::path& path = ".env");

} // namespace cha
