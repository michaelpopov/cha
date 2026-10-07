#include "support/test_workspace.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr const char usage[] =
    "usage: cha_prepare_test_vault [--provider-port PORT] CONFIG_DIR\n";

// Points the default character at an HTTP provider on 127.0.0.1:PORT and
// turns off session naming, so every provider request is a character reply.
void use_net_provider(const cha::test::TestWorkspace& workspace, int port) {
    const auto session = workspace.root() / "system/session";
    std::filesystem::create_directories(session);
    std::ofstream(session / "config.toml") << "naming_provider = \"test\"\n";
    workspace.write_provider(
        "remote",
        "host = \"127.0.0.1\"\nport = " + std::to_string(port)
            + "\nhttps = false\nmode = \"net\"\nmodel = \"fake\"\n"
              "api = \"chat_completions\"\nstream = false\ntimeout_s = 20\n");
    workspace.write_character_config(
        "display_name = \"Guide\"\nprovider = \"remote\"\n");
    std::ofstream(workspace.root() / "system" / "assistant" / "character.toml")
        << "display_name = \"Assistant\"\nprovider = \"remote\"\n";
}

std::optional<int> parse_port(std::string_view text) {
    int port = 0;
    for (const char digit : text) {
        if (digit < '0' || digit > '9' || port > 65535) return std::nullopt;
        port = port * 10 + (digit - '0');
    }
    if (port <= 0 || port > 65535) return std::nullopt;
    return port;
}

} // namespace

int main(int argc, char** argv) {
    std::optional<int> provider_port;
    if (argc == 4 && std::string_view(argv[1]) == "--provider-port") {
        provider_port = parse_port(argv[2]);
        if (!provider_port) {
            std::cerr << usage;
            return 2;
        }
    } else if (argc != 2) {
        std::cerr << usage;
        return 2;
    }
    const std::filesystem::path destination(argv[argc - 1]);
    std::filesystem::create_directories(destination);
    cha::test::TestWorkspace workspace;
    if (provider_port) use_net_provider(workspace, *provider_port);
    const std::filesystem::path database = destination / "test.sqlite3";
    (void)cha::test::import_test_database(workspace.root(), database);
    {
        std::ofstream app(destination / "app.toml");
        app << "vault = \"Test\"\n"
            << "[logging]\nfile = \"runtime.log\"\nlevel = \"off\"\n";
    }
    {
        std::ofstream vault(destination / "test.toml");
        vault << "vault_name = \"Test\"\n"
              << "data = " << nlohmann::json(database.string()).dump() << "\n";
    }
    return 0;
}
