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

struct StoredApplicationSettings {
    std::string host;
    int port{};
};

// Reads materialized root app.toml. host and port are required. The stored
// form does not accept workspace or backup_dir.
StoredApplicationSettings load_stored_application_settings(
    const std::filesystem::path& config_file);

// Resolves the application root, reads app.toml when present, and applies
// command-line overrides. Throws a user-facing error for invalid arguments or
// missing settings.
ApplicationConfig load_application_config(
    int argc,
    const char* const* argv);

// Customer-facing, so it omits --test-idle-grace-ms. That option shortens the
// runtime's idle unload so the browser suite can observe a real one; it is
// accepted but deliberately not advertised to someone who mistyped an option.
inline constexpr const char web_usage[] =
    "Usage: chaweb [--root PATH] [--config PATH] [--host HOST] "
    "[--port PORT] [--workspace PATH]";

} // namespace cha::web
