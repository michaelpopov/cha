#include "web/application_config.h"

#include "util/path_name.h"

#include <toml++/toml.hpp>

#include <charconv>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace cha::web {
namespace {

struct ParsedOptions {
    std::optional<std::filesystem::path> database;
    std::optional<std::filesystem::path> import_directory;
    std::optional<std::filesystem::path> export_directory;
    std::optional<std::filesystem::path> root;
    std::optional<std::string> host;
    std::optional<int> port;
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

std::filesystem::path normalize_cli_path(
    std::string_view option,
    std::string_view value) {
    if (value.empty()) {
        throw argument_error(
            "Option '" + std::string(option) + "' requires a non-empty path.");
    }
    return std::filesystem::weakly_canonical(
        std::filesystem::absolute(path_from_utf8(value)));
}

void assign_unique_path(
    std::optional<std::filesystem::path>& destination,
    std::string_view option,
    std::string_view value) {
    if (destination) {
        throw argument_error(
            "Option '" + std::string(option) + "' was provided more than once.");
    }
    destination = normalize_cli_path(option, value);
}

void reject_runtime_option_in_offline_mode(
    std::string_view option,
    std::string_view offline) {
    throw argument_error(
        "Option '" + std::string(option) + "' is a runtime option and cannot "
        "be used with " + std::string(offline) + ".");
}

ParsedOptions parse_arguments(int argc, const char* const* argv) {
    ParsedOptions result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option(argv[index]);
        if (option == "--config") {
            throw argument_error(
                "Unknown option '--config'. The old --config APP_TOML option "
                "has been removed; pass --data DATABASE.");
        }
        if (option == "--workspace") {
            throw argument_error(
                "Unknown option '--workspace'. The --workspace option has "
                "been removed; pass --data DATABASE.");
        }
        if (option != "--data" && option != "--import" && option != "--export"
            && option != "--root" && option != "--host" && option != "--port"
            && option != "--test-idle-grace-ms") {
            throw argument_error("Unknown option '" + std::string(option) + "'.");
        }
        if (++index >= argc) {
            throw argument_error(
                "Option '" + std::string(option) + "' requires a value.");
        }
        const std::string_view value(argv[index]);
        if (option == "--data") {
            assign_unique_path(result.database, option, value);
        } else if (option == "--import") {
            assign_unique_path(result.import_directory, option, value);
        } else if (option == "--export") {
            assign_unique_path(result.export_directory, option, value);
        } else if (option == "--root") {
            assign_unique_path(result.root, option, value);
        } else if (option == "--host") {
            if (result.host) {
                throw argument_error(
                    "Option '--host' was provided more than once.");
            }
            if (value.empty()) {
                throw argument_error("Option '--host' requires a non-empty value.");
            }
            result.host = std::string(value);
        } else if (option == "--port") {
            if (result.port) {
                throw argument_error(
                    "Option '--port' was provided more than once.");
            }
            result.port = parse_port(value);
        } else {
            if (result.test_idle_grace_ms) {
                throw argument_error(
                    "Option '--test-idle-grace-ms' was provided more than once.");
            }
            result.test_idle_grace_ms = parse_test_idle_grace(value);
        }
    }
    return result;
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

ApplicationCommand parse_application_command(
    int argc,
    const char* const* argv) {
    const ParsedOptions options = parse_arguments(argc, argv);
    if (!options.database) {
        throw argument_error(
            "Missing --data DATABASE. Every mode needs a CHA database. If "
            "the file does not exist yet, import a workspace first:\n"
            "  chaweb --data DATABASE --import SOURCE_DIRECTORY");
    }
    if (options.import_directory && options.export_directory) {
        throw argument_error("--import and --export are mutually exclusive.");
    }

    const bool offline =
        options.import_directory.has_value() || options.export_directory.has_value();
    if (offline) {
        const std::string_view offline_option = options.import_directory
            ? "--import" : "--export";
        if (options.root) {
            reject_runtime_option_in_offline_mode("--root", offline_option);
        }
        if (options.host) {
            reject_runtime_option_in_offline_mode("--host", offline_option);
        }
        if (options.port) {
            reject_runtime_option_in_offline_mode("--port", offline_option);
        }
        if (options.test_idle_grace_ms) {
            reject_runtime_option_in_offline_mode(
                "--test-idle-grace-ms", offline_option);
        }
        return {
            .database = *options.database,
            .import_directory = options.import_directory,
            .export_directory = options.export_directory,
        };
    }

    const std::filesystem::path root = options.root
        ? *options.root
        : std::filesystem::absolute(executable_directory()).lexically_normal();
    return {
        .database = *options.database,
        .root = root,
        .host = options.host,
        .port = options.port,
        .test_idle_grace_ms = options.test_idle_grace_ms,
    };
}

} // namespace cha::web
