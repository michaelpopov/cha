#include "web/application_config.h"

#include "util/path_name.h"
#include "util/public_name.h"
#include "util/text.h"

#include <toml++/toml.hpp>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cha::web {
namespace {

struct ParsedOptions {
    std::optional<std::filesystem::path> config;
    std::optional<std::string> vault;
    std::optional<std::filesystem::path> import_directory;
    std::optional<std::filesystem::path> export_directory;
    bool upload{};
    bool download{};
    std::optional<std::filesystem::path> root;
    std::optional<int> test_idle_grace_ms;
};

struct LoadedVault {
    VaultDefinition definition;
    std::filesystem::path source;
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

void assign_unique_vault(
    std::optional<std::string>& destination,
    std::string_view value) {
    if (destination) {
        throw argument_error("Option '--vault' was provided more than once.");
    }
    if (value.empty()) {
        throw argument_error("Option '--vault' requires a non-empty name.");
    }
    destination = std::string(value);
}

ParsedOptions parse_arguments(int argc, const char* const* argv) {
    ParsedOptions result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        const std::size_t equals = argument.find('=');
        const std::string_view option = argument.substr(0, equals);
        if (option != "--config" && option != "--vault" && option != "--import"
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
        } else if (option == "--vault") {
            assign_unique_vault(result.vault, value);
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
    const std::filesystem::path& source,
    std::initializer_list<std::string_view> allowed,
    std::string_view section,
    std::string_view kind) {
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
                std::string(kind) + " '" + utf8_path(source) + "' "
                + std::string(section) + " contains unknown field '"
                + std::string(key.str()) + "'.");
        }
    }
}

const toml::table& required_table(
    const toml::table& parent,
    const std::filesystem::path& source,
    std::string_view name,
    std::string_view kind) {
    const toml::table* const result = parent[name].as_table();
    if (result == nullptr) {
        throw std::runtime_error(
            std::string(kind) + " '" + utf8_path(source)
            + "' requires [" + std::string(name) + "].");
    }
    return *result;
}

std::string required_string(
    const toml::table& table,
    const std::filesystem::path& source,
    std::string_view name,
    std::string_view kind) {
    const std::optional<std::string> value = table[name].value<std::string>();
    if (!value || value->empty()) {
        throw std::runtime_error(
            std::string(kind) + " '" + utf8_path(source)
            + "' requires a non-empty string '" + std::string(name) + "'.");
    }
    return *value;
}

std::filesystem::path resolve_config_path(
    const std::filesystem::path& directory,
    const std::filesystem::path& source,
    std::string_view field,
    std::string_view value,
    std::string_view kind) {
    std::filesystem::path path = path_from_utf8(value);
    if (path.is_relative()) path = directory / path;
    if (path.empty()) {
        throw std::runtime_error(
            std::string(kind) + " '" + utf8_path(source)
            + "' requires a non-empty path '" + std::string(field) + "'.");
    }
    return std::filesystem::weakly_canonical(std::filesystem::absolute(path));
}

toml::table parse_toml_file(
    const std::filesystem::path& source,
    std::string_view kind) {
    std::ifstream input(source, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            "Failed to read " + std::string(kind) + " '"
            + utf8_path(source) + "'.");
    }
    try {
        return toml::parse(input, utf8_path(source));
    } catch (const toml::parse_error& error) {
        throw std::runtime_error(
            "Failed to parse " + std::string(kind) + " '"
            + utf8_path(source) + "': " + std::string(error.description()));
    }
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

bool modify_contains(
    const std::filesystem::path& modify,
    const std::filesystem::path& candidate) {
    return modify == candidate || path_is_under(modify, candidate);
}

bool modify_roots_overlap(
    const std::filesystem::path& left,
    const std::filesystem::path& right) {
    return modify_contains(left, right) || modify_contains(right, left);
}

std::string vault_collision(
    const LoadedVault& left,
    std::string_view left_field,
    const std::filesystem::path& left_path,
    const LoadedVault& right,
    std::string_view right_field,
    const std::filesystem::path& right_path) {
    return "Vault '" + left.definition.name + "' in '"
        + utf8_path(left.source) + "' field '" + std::string(left_field)
        + "' ('" + utf8_path(left_path) + "') collides with vault '"
        + right.definition.name + "' in '" + utf8_path(right.source)
        + "' field '" + std::string(right_field) + "' ('"
        + utf8_path(right_path) + "').";
}

LoadedVault load_vault_definition(
    const std::filesystem::path& directory,
    const std::filesystem::path& source) {
    constexpr std::string_view kind = "vault definition";
    const toml::table root = parse_toml_file(source, kind);
    reject_unknown_fields(
        root, source, {"vault_name", "data", "mirror", "modify"}, "root", kind);
    const std::string name =
        required_string(root, source, "vault_name", kind);
    validate_public_name(name, "vault_name", source);
    const std::string data = required_string(root, source, "data", kind);
    LoadedVault loaded;
    loaded.source = source;
    loaded.definition.name = name;
    loaded.definition.data =
        resolve_config_path(directory, source, "data", data, kind);
    if (root.contains("mirror")) {
        const std::string value = required_string(root, source, "mirror", kind);
        loaded.definition.mirror =
            resolve_config_path(directory, source, "mirror", value, kind);
    }
    if (root.contains("modify")) {
        const std::string value = required_string(root, source, "modify", kind);
        loaded.definition.modify =
            resolve_config_path(directory, source, "modify", value, kind);
    }
    return loaded;
}

void validate_vault_registry(
    const std::filesystem::path& directory,
    const std::vector<LoadedVault>& vaults) {
    if (vaults.empty()) {
        throw std::runtime_error(
            "Configuration directory '" + utf8_path(directory)
            + "' contains no vault definitions.");
    }
    for (std::size_t left = 0; left < vaults.size(); ++left) {
        for (std::size_t right = left + 1; right < vaults.size(); ++right) {
            const LoadedVault& a = vaults[left];
            const LoadedVault& b = vaults[right];
            if (same_vault_name(a.definition.name, b.definition.name)) {
                throw std::runtime_error(
                    "Vault '" + a.definition.name + "' in '"
                    + utf8_path(a.source) + "' duplicates vault '"
                    + b.definition.name + "' in '" + utf8_path(b.source)
                    + "'.");
            }
            if (a.definition.data == b.definition.data) {
                throw std::runtime_error(
                    vault_collision(
                        a, "data", a.definition.data,
                        b, "data", b.definition.data));
            }
            if (a.definition.data.filename() == b.definition.data.filename()) {
                throw std::runtime_error(
                    vault_collision(
                        a, "data", a.definition.data,
                        b, "data", b.definition.data)
                    + " Database filenames must be unique.");
            }
            if (a.definition.modify && b.definition.modify
                && modify_roots_overlap(
                       *a.definition.modify, *b.definition.modify)) {
                throw std::runtime_error(
                    vault_collision(
                        a, "modify", *a.definition.modify,
                        b, "modify", *b.definition.modify));
            }
        }
    }
    for (const LoadedVault& vault : vaults) {
        if (!vault.definition.modify) continue;
        const std::filesystem::path& modify = *vault.definition.modify;
        if (modify_contains(modify, directory)) {
            throw std::runtime_error(
                "Vault '" + vault.definition.name + "' in '"
                + utf8_path(vault.source) + "' field 'modify' ('"
                + utf8_path(modify)
                + "') must not equal or contain the configuration directory '"
                + utf8_path(directory) + "'.");
        }
        for (const LoadedVault& other : vaults) {
            if (modify_contains(modify, other.definition.data)) {
                throw std::runtime_error(
                    vault_collision(
                        vault, "modify", modify,
                        other, "data", other.definition.data));
            }
        }
    }
}

void reject_runtime_option_in_offline_mode(
    std::string_view option,
    std::string_view offline) {
    throw argument_error(
        "Option '" + std::string(option) + "' is a runtime option and cannot "
        "be used with " + std::string(offline) + ".");
}

} // namespace

bool same_vault_name(std::string_view left, std::string_view right) {
    return fold_ascii(left) == fold_ascii(right);
}

const VaultDefinition* find_vault(
    const std::vector<VaultDefinition>& vaults, std::string_view name) {
    for (const VaultDefinition& vault : vaults) {
        if (same_vault_name(vault.name, name)) return &vault;
    }
    return nullptr;
}

ConfigurationDirectory load_configuration_directory(
    const std::filesystem::path& directory) {
    if (!std::filesystem::is_directory(directory)) {
        throw std::runtime_error(
            "Configuration directory '" + utf8_path(directory)
            + "' requires an existing directory.");
    }
    const std::filesystem::path root =
        std::filesystem::weakly_canonical(std::filesystem::absolute(directory));
    constexpr std::string_view app_kind = "application config";
    const std::filesystem::path app_file = root / "app.toml";
    const toml::table app = parse_toml_file(app_file, app_kind);
    reject_unknown_fields(
        app, app_file, {"vault", "web", "logging"}, "root", app_kind);
    const std::string configured_vault =
        required_string(app, app_file, "vault", app_kind);
    const toml::table& web = required_table(app, app_file, "web", app_kind);
    reject_unknown_fields(web, app_file, {"host", "port"}, "[web]", app_kind);
    const std::string host = required_string(web, app_file, "host", app_kind);
    const std::optional<int> port = web["port"].value<int>();
    if (!port || *port < 0 || *port > 65535) {
        throw std::runtime_error(
            std::string(app_kind) + " '" + utf8_path(app_file)
            + "' requires an integer 'port' between 0 and 65535 in [web].");
    }
    const toml::table& logging =
        required_table(app, app_file, "logging", app_kind);
    reject_unknown_fields(
        logging, app_file, {"file", "level"}, "[logging]", app_kind);
    const std::string log_file =
        required_string(logging, app_file, "file", app_kind);
    const std::string log_level =
        required_string(logging, app_file, "level", app_kind);

    std::vector<std::filesystem::path> vault_files;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;
        const std::string filename = utf8_path(entry.path().filename());
        if (filename == "app.toml") continue;
        if (filename.size() < 5 || !filename.ends_with(".toml")) continue;
        vault_files.push_back(entry.path());
    }
    std::sort(vault_files.begin(), vault_files.end());

    std::vector<LoadedVault> loaded;
    loaded.reserve(vault_files.size());
    for (const std::filesystem::path& file : vault_files) {
        loaded.push_back(load_vault_definition(root, file));
    }
    validate_vault_registry(root, loaded);

    std::vector<VaultDefinition> vaults;
    vaults.reserve(loaded.size());
    for (LoadedVault& vault : loaded) {
        vaults.push_back(std::move(vault.definition));
    }
    std::sort(
        vaults.begin(),
        vaults.end(),
        [](const VaultDefinition& left, const VaultDefinition& right) {
            return fold_ascii(left.name) < fold_ascii(right.name);
        });

    const VaultDefinition* const startup = find_vault(vaults, configured_vault);
    if (startup == nullptr) {
        throw std::runtime_error(
            std::string(app_kind) + " '" + utf8_path(app_file)
            + "' field 'vault' does not name a discovered vault.");
    }

    return {
        .directory = root,
        .startup_vault = startup->name,
        .vaults = std::move(vaults),
        .host = host,
        .port = *port,
        .log_file = resolve_config_path(
            root, app_file, "logging.file", log_file, app_kind),
        .log_level = log_level,
    };
}

ApplicationCommand parse_application_command(
    int argc,
    const char* const* argv) {
    const ParsedOptions options = parse_arguments(argc, argv);
    if (!options.config) {
        throw argument_error("Missing --config=CONFIG_DIR.");
    }
    const int offline_count = static_cast<int>(options.import_directory.has_value())
        + static_cast<int>(options.export_directory.has_value())
        + static_cast<int>(options.upload)
        + static_cast<int>(options.download);
    if (offline_count > 1) {
        throw argument_error(
            "--import, --export, --upload, and --download are mutually exclusive.");
    }

    if (!std::filesystem::is_directory(*options.config)) {
        throw argument_error(
            "Option '--config' requires an existing directory.");
    }

    const bool offline = offline_count != 0;
    if (offline) {
        const std::string_view offline_option = options.import_directory
            ? "--import"
            : options.export_directory ? "--export"
            : options.upload ? "--upload"
            : "--download";
        if (!options.vault) {
            throw argument_error(
                "Option '--vault' is required with "
                + std::string(offline_option) + ".");
        }
        if (options.root) {
            reject_runtime_option_in_offline_mode("--root", offline_option);
        }
        if (options.test_idle_grace_ms) {
            reject_runtime_option_in_offline_mode(
                "--test-idle-grace-ms", offline_option);
        }
    } else if (options.vault) {
        throw argument_error(
            "Option '--vault' cannot be used in server mode.");
    }

    const ConfigurationDirectory settings =
        load_configuration_directory(*options.config);
    if (options.import_directory
        && path_is_under(*options.import_directory, settings.directory)) {
        throw argument_error(
            "Configuration directory '" + utf8_path(settings.directory)
            + "' must be outside the imported workspace.");
    }

    const VaultDefinition* selected = nullptr;
    if (offline) {
        selected = find_vault(settings.vaults, *options.vault);
        if (selected == nullptr) {
            throw argument_error(
                "Unknown vault '" + *options.vault + "'.");
        }
    } else {
        selected = find_vault(settings.vaults, settings.startup_vault);
        if (selected == nullptr) {
            throw std::runtime_error(
                "Application config '"
                + utf8_path(settings.directory / "app.toml")
                + "' field 'vault' does not name a discovered vault.");
        }
    }

    const std::filesystem::path root = offline
        ? std::filesystem::path{}
        : options.root.value_or(
              std::filesystem::absolute(executable_directory()).lexically_normal());
    return {
        .config_directory = settings.directory,
        .vaults = settings.vaults,
        .vault = *selected,
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
