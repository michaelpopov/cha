#pragma once

#include <filesystem>
#include <string>

namespace cha::web {

struct ApplicationConfig {
    std::filesystem::path root;
    std::filesystem::path config_file;
    std::string host;
    int port{};
    std::filesystem::path workspace;
};

// Resolves the application root, reads app.toml when present, and applies
// command-line overrides. Throws a user-facing error for invalid arguments or
// missing settings.
ApplicationConfig load_application_config(
    int argc,
    const char* const* argv);

inline constexpr const char web_usage[] =
    "Usage: chaweb [--root PATH] [--config PATH] [--host HOST] "
    "[--port PORT] [--workspace PATH]";

} // namespace cha::web
