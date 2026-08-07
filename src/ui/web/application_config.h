#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace cha::web {

struct ApplicationConfig {
    std::filesystem::path root;
    std::filesystem::path config_file;
    std::string host;
    int port{};
    std::filesystem::path workspace;
    // Browser automation uses this command-line-only seam to prove a real
    // disconnect/unload/reopen cycle without adding thirty seconds per run.
    std::optional<int> test_idle_grace_ms;
};

// Resolves the application root, reads app.toml when present, and applies
// command-line overrides. Throws a user-facing error for invalid arguments or
// missing settings.
ApplicationConfig load_application_config(
    int argc,
    const char* const* argv);

inline constexpr const char web_usage[] =
    "Usage: chaweb [--root PATH] [--config PATH] [--host HOST] "
    "[--port PORT] [--workspace PATH] [--test-idle-grace-ms MS]";

} // namespace cha::web
