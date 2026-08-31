#include "web/application_config.h"

#include "util/path_name.h"

#include <toml++/toml.hpp>

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cha::web {
namespace {

struct Overrides {
    std::optional<std::filesystem::path> root;
    std::optional<std::filesystem::path> config_file;
    std::optional<std::string> host;
    std::optional<int> port;
    std::optional<std::filesystem::path> workspace;
    std::optional<int> test_idle_grace_ms;
};

std::runtime_error argument_error(std::string message) {
    return std::runtime_error(std::move(message) + "\n" + web_usage);
}

int parse_port(std::string_view value) {
    int port{};
    const auto parsed = std::from_chars(
        value.data(), value.data() + value.size(), port);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()
        || port < 1 || port > 65535) {
        throw argument_error(
            "Invalid --port value '" + std::string(value)
            + "'; expected an integer between 1 and 65535.");
    }
    return port;
}

int parse_test_idle_grace(std::string_view value) {
    int milliseconds{};
    const auto parsed = std::from_chars(
        value.data(), value.data() + value.size(), milliseconds);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()
        || milliseconds < 1) {
        throw argument_error(
            "Invalid --test-idle-grace-ms value '" + std::string(value)
            + "'; expected a positive integer.");
    }
    return milliseconds;
}

Overrides parse_arguments(int argc, const char* const* argv) {
    Overrides result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option(argv[index]);
        if (option != "--root" && option != "--config"
            && option != "--host" && option != "--port"
            && option != "--workspace"
            && option != "--test-idle-grace-ms") {
            throw argument_error("Unknown option '" + std::string(option) + "'.");
        }
        if (++index >= argc) {
            throw argument_error(
                "Option '" + std::string(option) + "' requires a value.");
        }
        const std::string_view value(argv[index]);
        if (option == "--root") result.root = path_from_utf8(value);
        else if (option == "--config") result.config_file = path_from_utf8(value);
        else if (option == "--host") result.host = std::string(value);
        else if (option == "--port") result.port = parse_port(value);
        else if (option == "--workspace") result.workspace = path_from_utf8(value);
        else result.test_idle_grace_ms = parse_test_idle_grace(value);
    }
    return result;
}

template <typename Value>
Value require_setting(
    std::optional<Value> value,
    std::string_view name,
    std::string_view option,
    const std::filesystem::path& config_file) {
    if (value) return std::move(*value);
    throw std::runtime_error(
        "Missing application setting '" + std::string(name) + "': set it in '"
        + utf8_path(config_file) + "' or pass " + std::string(option) + ".");
}

std::filesystem::path user_home_directory() {
#ifdef _WIN32
    const char* const value = std::getenv("USERPROFILE");
#else
    const char* const value = std::getenv("HOME");
#endif
    if (value == nullptr || *value == '\0') {
        throw std::runtime_error(
            "Could not determine the user's home directory for workspace backups.");
    }
    return path_from_utf8(value);
}

void read_host_and_port(
    const toml::table& table,
    const std::filesystem::path& config_file,
    std::optional<std::string>& host,
    std::optional<int>& port) {
    host = table["host"].value<std::string>();
    port = table["port"].value<int>();
    if (host && host->empty()) {
        throw std::runtime_error(
            "Application config '" + utf8_path(config_file)
            + "' requires a non-empty string 'host'.");
    }
    if (port && (*port < 1 || *port > 65535)) {
        throw std::runtime_error(
            "Application config '" + utf8_path(config_file)
            + "' requires an integer 'port' between 1 and 65535.");
    }
}

toml::table parse_application_table(const std::filesystem::path& config_file) {
    std::ifstream input(config_file, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            "Failed to read application config '" + utf8_path(config_file)
            + "'.");
    }
    return toml::parse(input, utf8_path(config_file));
}

} // namespace

StoredApplicationSettings load_stored_application_settings(
    const std::filesystem::path& config_file) {
    const toml::table table = parse_application_table(config_file);
    if (table.contains("workspace") || table.contains("backup_dir")) {
        throw std::runtime_error(
            "Application config '" + utf8_path(config_file)
            + "' must not contain 'workspace' or 'backup_dir'.");
    }
    std::optional<std::string> host;
    std::optional<int> port;
    read_host_and_port(table, config_file, host, port);
    if (!host) {
        throw std::runtime_error(
            "Application config '" + utf8_path(config_file)
            + "' requires a non-empty string 'host'.");
    }
    if (!port) {
        throw std::runtime_error(
            "Application config '" + utf8_path(config_file)
            + "' requires an integer 'port' between 1 and 65535.");
    }
    return {.host = std::move(*host), .port = *port};
}

ApplicationConfig load_application_config(
    int argc,
    const char* const* argv) {
    const Overrides overrides = parse_arguments(argc, argv);
    const std::filesystem::path root = std::filesystem::absolute(
        overrides.root.value_or(executable_directory())).lexically_normal();
    const bool explicit_config = overrides.config_file.has_value();
    const std::filesystem::path config_file = std::filesystem::absolute(
        overrides.config_file.value_or(root / "app.toml")).lexically_normal();

    std::optional<std::string> host;
    std::optional<int> port;
    std::optional<std::filesystem::path> workspace;
    std::optional<std::filesystem::path> backup_dir;
    if (std::filesystem::exists(config_file)) {
        const toml::table table = parse_application_table(config_file);
        read_host_and_port(table, config_file, host, port);
        if (const auto configured = table["workspace"].value<std::string>()) {
            if (configured->empty()) {
                throw std::runtime_error(
                    "Application config '" + utf8_path(config_file)
                    + "' requires a non-empty string 'workspace'.");
            }
            workspace = path_from_utf8(*configured);
            if (workspace->is_relative()) {
                workspace = config_file.parent_path() / *workspace;
            }
        }
        const auto configured = table["backup_dir"].value<std::string>();
        if (configured) {
            if (configured->empty()) {
                throw std::runtime_error(
                    "Application config '" + utf8_path(config_file)
                    + "' requires a non-empty string 'backup_dir'.");
            }
            backup_dir = path_from_utf8(*configured);
            if (backup_dir->is_relative()) {
                backup_dir = config_file.parent_path() / *backup_dir;
            }
        }
    } else if (explicit_config) {
        throw std::runtime_error(
            "Application config '" + utf8_path(config_file) + "' does not exist.");
    }

    if (overrides.host) host = overrides.host;
    if (overrides.port) port = overrides.port;
    if (overrides.workspace) workspace = overrides.workspace;

    std::filesystem::path workspace_setting = require_setting(
        std::move(workspace), "workspace", "--workspace", config_file);
    if (workspace_setting.empty()) {
        throw std::runtime_error("Application setting 'workspace' must not be empty.");
    }
    std::filesystem::path resolved_workspace = std::filesystem::absolute(
        std::move(workspace_setting))
        .lexically_normal();
    if (!std::filesystem::is_directory(resolved_workspace)
        || !std::filesystem::is_regular_file(resolved_workspace / "workspace.toml")) {
        throw std::runtime_error(
            "Workspace '" + utf8_path(resolved_workspace)
            + "' is not a valid workspace with workspace.toml; correct 'workspace' "
              "in the application config or pass --workspace.");
    }
    std::string resolved_host = require_setting(
        std::move(host), "host", "--host", config_file);
    if (resolved_host.empty()) {
        throw std::runtime_error("Application setting 'host' must not be empty.");
    }
    const int resolved_port = require_setting(
        port, "port", "--port", config_file);
    std::filesystem::path resolved_backup_dir = std::filesystem::absolute(
        backup_dir.value_or(user_home_directory())).lexically_normal();
    return {
        .root = root,
        .config_file = config_file,
        .host = std::move(resolved_host),
        .port = resolved_port,
        .workspace = std::move(resolved_workspace),
        .backup_dir = std::move(resolved_backup_dir),
        .test_idle_grace_ms = overrides.test_idle_grace_ms,
    };
}

} // namespace cha::web
