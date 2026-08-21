#include "support/test_workspace.h"

#include <chrono>
#include <fstream>
#include <string>

namespace cha::test {

TestWorkspace::TestWorkspace()
    : root_(
          std::filesystem::temp_directory_path()
          / ("cha_web_workspace_"
             + std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()))) {
    const auto definition = root_ / "characters" / "guide";
    const auto member = root_ / "forums" / "lobby" / "members" / "guide";
    std::filesystem::create_directories(definition);
    std::filesystem::create_directories(member);
    std::filesystem::create_directories(root_ / "personas");
    write_provider("test", "host = \"test\"\nport = 1\nmode = \"test\"\nmodel = \"fake\"\n");
    std::filesystem::create_directories(root_ / "system" / "assistant");
    std::ofstream(root_ / "system" / "assistant" / "character.toml")
        << "display_name = \"Assistant\"\nprovider = \"test\"\n";
    write_workspace_config();
    std::filesystem::create_directories(root_ / "web" / "assets");
    std::ofstream(root_ / "web" / "index.html")
        << "<!doctype html><html><head><title>cha</title></head>"
           "<body>test shell</body></html>\n";
    std::ofstream(root_ / "web" / "assets" / "app.js")
        << "console.log('test asset');\n";
    std::ofstream(root_ / "forums" / "lobby" / "config.toml")
        << "display_name = \"The Lobby\"\n";
    std::ofstream(root_ / "forums" / "lobby" / "FORUM.md")
        << "Forum instructions\n";
    write_character_defaults("# Provider selection is per character.\n");
    write_character_config("display_name = \"Guide\"\nprovider = \"test\"\n");
    std::ofstream(definition / "CHARACTER.md") << "Character instructions\n";
    add_persona("reader", "Reader");
}

TestWorkspace::~TestWorkspace() {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
}

void TestWorkspace::write_workspace_config(std::string_view log_level) const {
    std::ofstream(root_ / "workspace.toml")
        << "[logging]\n"
           "file = \"logs/cha.log\"\n"
           "level = \"" << log_level << "\"\n";
}

void TestWorkspace::write_provider(
    std::string_view name,
    std::string_view contents) const {
    const std::filesystem::path directory =
        root_ / "system" / "providers" / std::string(name);
    std::filesystem::create_directories(directory);
    std::ofstream(directory / "config.toml") << contents;
}

void TestWorkspace::write_style(
    std::string_view name,
    std::string_view contents) const {
    const std::filesystem::path directory =
        root_ / "system" / "styles" / std::string(name);
    std::filesystem::create_directories(directory);
    std::ofstream(directory / "config.toml") << contents;
}

void TestWorkspace::write_character_config(std::string_view contents) const {
    std::ofstream(
        root_ / "characters" / "guide" / "character.toml")
        << contents;
}

void TestWorkspace::write_character_defaults(std::string_view contents) const {
    std::ofstream(
        root_ / "forums" / "lobby" / "members" / "character_defaults.toml")
        << contents;
}

void TestWorkspace::add_persona(
    std::string_view id,
    std::string_view display_name,
    std::string_view prompt) const {
    const std::filesystem::path directory = root_ / "personas" / std::string(id);
    std::filesystem::create_directories(directory);
    std::ofstream(directory / "persona.toml")
        << "display_name = \"" << display_name << "\"\n";
    if (!prompt.empty()) std::ofstream(directory / "PERSONA.md") << prompt;
}

void TestWorkspace::add_character(
    std::string_view id,
    std::string_view display_name) const {
    const std::filesystem::path directory = root_ / "characters" / std::string(id);
    std::filesystem::create_directories(directory);
    std::ofstream(directory / "character.toml")
        << "display_name = \"" << display_name << "\"\n"
           "provider = \"test\"\n";
    std::ofstream(directory / "CHARACTER.md") << display_name << " instructions\n";
}

void TestWorkspace::add_forum(
    std::string_view id,
    std::string_view display_name,
    std::string_view member) const {
    const std::filesystem::path directory = root_ / "forums" / std::string(id);
    std::filesystem::create_directories(directory / "members" / std::string(member));
    std::ofstream(directory / "config.toml")
        << "display_name = \"" << display_name << "\"\n";
    std::ofstream(directory / "FORUM.md") << display_name << " forum instructions\n";
    std::ofstream(directory / "members" / "character_defaults.toml")
        << "# Provider selection is per character.\n";
}

} // namespace cha::test
