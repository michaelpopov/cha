#pragma once

#include "agents/config.h"

#include <filesystem>

namespace cha {

// Loads one persisted agent configuration from TOML.
Config load_config(const std::filesystem::path& path);

} // namespace cha
