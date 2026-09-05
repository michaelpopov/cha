#include "web/application_config.h"

#include "util/path_name.h"

#include <toml++/toml.hpp>

#include <charconv>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace cha::web {
namespace {

struct ParsedOptions {
    std::optional<std::filesystem::path> config;
    std::optional<std::filesystem::path> import_directory;
    std::optional<std::filesystem::path> export_directory;
    bool upload{};
    bool download{};
    std::optional<std::filesystem::path> root;
    std::optional<int> test_idle_grace_ms;
};

struct ApplicationSettings {
    std::filesystem::path database;
    std::optional<std::filesystem::path> mirror;
    std::optional<std::filesystem::path> modify;
    std::string host;
    int port{};
    std::filesystem::path log_file;
    std::string log_level;
};

std::runtime_error argument_error(std::string message) {
    return std::runtime_error(std::move(message) + "\n" + web_usage);
}

int parse_positive_integer(
    std::string_view option,
    std::string_view value) {
    int result{};
    const auto parsed = std::from_chars(
        value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()
        || result < 1) {
        throw argument_error(
            "Invalid " + std::string(option) + " value '" + std::string(value)
            + "'; expected a positive integer.");
    }
    return result;
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

void assign_unique_flag(
    bool& destination,
    std::string_view option,
    bool has_value) {
    if (has_value) {
        throw argument_error(
            "Option '" + std::string(option) + "' does not take a value.");
    }
    if (destination) {
        throw argument_error(
            "Option '" + std::string(option) + "' was provided more than once.");
    }
    destination = true;
}

ParsedOptions parse_arguments(int argc, const char* const* argv) {
    ParsedOptions result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        const std::size_t equals = argument.find('=');
        const std::string_view option = argument.substr(0, equals);
        if (option != "--config" && option != "--import"
            && option != "--export" && option != "--root"
            && option != "--upload" && option != "--download"
            && option != "--test-idle-grace-ms") {
            throw argument_error("Unknown option '" + std::string(option) + "'.");
        }

        if (option == "--upload" || option == "--download") {
            bool& flag = option == "--upload" ? result.upload : result.download;
            assign_unique_flag(
                flag, option, equals != std::string_view::npos);
            continue;
        }

        std::string_view value;
        if (equals != std::string_view::npos) {
            value = argument.substr(equals + 1);
        } else {
            if (++index >= argc) {
                throw argument_error(
                    "Option '" + std::string(option) + "' requires a value.");
            }
            value = argv[index];
        }

        if (option == "--config") {
            assign_unique_path(result.config, option, value);
        } else if (option == "--import") {
            assign_unique_path(result.import_directory, option, value);
        } else if (option == "--export") {
            assign_unique_path(result.export_directory, option, value);
        } else if (option == "--root") {
            assign_unique_path(result.root, option, value);
        } else {
            if (result.test_idle_grace_ms) {
                throw argument_error(
                    "Option '--test-idle-grace-ms' was provided more than once.");
            }
            result.test_idle_grace_ms =
                parse_positive_integer(option, value);
        }
    }
    return result;
}

void reject_unknown_fields(
    const toml::table& table,
    const std::filesystem::path& config_file,
    std::initializer_list<std::string_view> allowed,
    std::string_view section) {
    for (const auto& [key, value] : table) {
        (void)value;
        bool found = false;
        for (const std::string_view name : allowed) {
            if (key.str() == name) {
                found = true;
                break;
            }
        }
        if (!found) {
            throw std::runtime_error(
                "Application config '" + utf8_path(config_file) + "' "
                + std::string(section) + " contains unknown field '"
                + std::string(key.str()) + "'.");
        }
    }
}

const toml::table& required_table(
    const toml::table& parent,
    const std::filesystem::path& config_file,
    std::string_view name) {
    const toml::table* const result = parent[name].as_table();
    if (result == nullptr) {
        throw std::runtime_error(
            "Application config '" + utf8_path(config_file)
            + "' requires [" + std::string(name) + "].");
    }
    return *result;
}

std::string required_string(
    const toml::table& table,
    const std::filesystem::path& config_file,
    std::string_view name) {
    const std::optional<std::string> value = table[name].value<std::string>();
    if (!value || value->empty()) {
        throw std::runtime_error(
            "Application config '" + utf8_path(config_file)
            + "' requires a non-empty string '" + std::string(name) + "'.");
    }
    return *value;
}

std::filesystem::path resolve_config_path(
    const std::filesystem::path& config_file,
    std::string_view field,
    std::string_view value) {
    std::filesystem::path path = path_from_utf8(value);
    if (path.is_relative()) path = config_file.parent_path() / path;
    if (path.empty()) {
        throw std::runtime_error(
            "Application config '" + utf8_path(config_file)
            + "' requires a non-empty path '" + std::string(field) + "'.");
    }
    return std::filesystem::weakly_canonical(std::filesystem::absolute(path));
}

ApplicationSettings load_application_settings(
    const std::filesystem::path& config_file) {
    std::ifstream input(config_file, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            "Failed to read application config '" + utf8_path(config_file) + "'.");
    }
    const toml::table root = toml::parse(input, utf8_path(config_file));
    reject_unknown_fields(
        root,
        config_file,
        {"data", "mirror", "modify", "web", "logging"},
        "root");

    const std::string data = required_string(root, config_file, "data");
    std::optional<std::filesystem::path> mirror;
    if (root.contains("mirror")) {
        const std::string value = required_string(root, config_file, "mirror");
        mirror = resolve_config_path(config_file, "mirror", value);
    }
    std::optional<std::filesystem::path> modify;
    if (root.contains("modify")) {
        const std::string value = required_string(root, config_file, "modify");
        modify = resolve_config_path(config_file, "modify", value);
    }
    const toml::table& web = required_table(root, config_file, "web");
    reject_unknown_fields(web, config_file, {"host", "port"}, "[web]");
    const std::string host = required_string(web, config_file, "host");
    const std::optional<int> port = web["port"].value<int>();
    if (!port || *port < 0 || *port > 65535) {
        throw std::runtime_error(
            "Application config '" + utf8_path(config_file)
            + "' requires an integer 'port' between 0 and 65535 in [web].");
    }

    const toml::table& logging = required_table(root, config_file, "logging");
    reject_unknown_fields(logging, config_file, {"file", "level"}, "[logging]");
    const std::string log_file = required_string(logging, config_file, "file");
    const std::string log_level = required_string(logging, config_file, "level");

    return {
        .database = resolve_config_path(config_file, "data", data),
        .mirror = std::move(mirror),
        .modify = std::move(modify),
        .host = host,
        .port = *port,
        .log_file = resolve_config_path(config_file, "logging.file", log_file),
        .log_level = log_level,
    };
}

void reject_runtime_option_in_offline_mode(
    std::string_view option,
    std::string_view offline) {
    throw argument_error(
        "Option '" + std::string(option) + "' is a runtime option and cannot "
        "be used with " + std::string(offline) + ".");
}

bool path_is_under(
    const std::filesystem::path& directory,
    const std::filesystem::path& candidate) {
    auto directory_part = directory.begin();
    auto candidate_part = candidate.begin();
    while (directory_part != directory.end()) {
        if (candidate_part == candidate.end()
            || *directory_part != *candidate_part) {
            return false;
        }
        ++directory_part;
        ++candidate_part;
    }
    return true;
}

} // namespace

ApplicationCommand parse_application_command(
    int argc,
    const char* const* argv) {
    const ParsedOptions options = parse_arguments(argc, argv);
    if (!options.config) {
        throw argument_error("Missing --config=CONFIG.");
    }
    const int offline_count = static_cast<int>(options.import_directory.has_value())
        + static_cast<int>(options.export_directory.has_value())
        + static_cast<int>(options.upload)
        + static_cast<int>(options.download);
    if (offline_count > 1) {
        throw argument_error(
            "--import, --export, --upload, and --download are mutually exclusive.");
    }
    if (options.import_directory
        && path_is_under(*options.import_directory, *options.config)) {
        throw argument_error(
            "Application config '" + utf8_path(*options.config)
            + "' must be outside the imported workspace.");
    }

    const bool offline =
        offline_count != 0;
    if (offline) {
        const std::string_view offline_option = options.import_directory
            ? "--import"
            : options.export_directory ? "--export"
            : options.upload ? "--upload"
            : "--download";
        if (options.root) {
            reject_runtime_option_in_offline_mode("--root", offline_option);
        }
        if (options.test_idle_grace_ms) {
            reject_runtime_option_in_offline_mode(
                "--test-idle-grace-ms", offline_option);
        }
    }

    const ApplicationSettings settings = load_application_settings(*options.config);
    if (settings.modify
        && (path_is_under(*settings.modify, *options.config)
            || path_is_under(*settings.modify, settings.database))) {
        throw std::runtime_error(
            "Application config '" + utf8_path(*options.config)
            + "' field 'modify' must not contain the config or database.");
    }
    const std::filesystem::path root = offline
        ? std::filesystem::path{}
        : options.root.value_or(
              std::filesystem::absolute(executable_directory()).lexically_normal());
    return {
        .database = settings.database,
        .mirror = settings.mirror,
        .modify = settings.modify,
        .import_directory = options.import_directory,
        .export_directory = options.export_directory,
        .upload = options.upload,
        .download = options.download,
        .root = root,
        .host = settings.host,
        .port = settings.port,
        .log_file = settings.log_file,
        .log_level = settings.log_level,
        .test_idle_grace_ms = options.test_idle_grace_ms,
    };
}

} // namespace cha::web
