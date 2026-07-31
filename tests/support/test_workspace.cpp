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
    const auto persona = root_ / "forums" / "lobby" / "personas" / "guide";
    std::filesystem::create_directories(persona);
    std::ofstream(root_ / "app.toml")
        << "host = \"127.0.0.1\"\n"
           "port = 8080\n"
           "[logging]\n"
           "file = \"logs/cha.log\"\n"
           "level = \"off\"\n";
    std::ofstream(root_ / "forums" / "lobby" / "config.toml")
        << "display_name = \"The Lobby\"\n";
    std::ofstream(root_ / "forums" / "lobby" / "USER.md")
        << "Forum instructions\n";
    std::ofstream(
        root_ / "forums" / "lobby" / "personas" / "persona_defaults.toml")
        << "host = \"test\"\n"
           "port = 1\n"
           "mode = \"test\"\n"
           "model = \"fake\"\n";
    write_persona_config("display_name = \"Guide\"\n");
    std::ofstream(persona / "SYSTEM.md") << "Persona instructions\n";
}

TestWorkspace::~TestWorkspace() {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
}

void TestWorkspace::write_persona_config(std::string_view contents) const {
    std::ofstream(
        root_ / "forums" / "lobby" / "personas" / "guide" / "persona.toml")
        << contents;
}

} // namespace cha::test
