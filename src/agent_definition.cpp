#include "agent_definition.h"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace cha {
namespace {

std::string read_prompt(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error("Failed to read prompt file '" + path.string() + "'");
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

} // namespace

AgentDefinition load_agent_definition(
    const std::filesystem::path& persona_directory,
    const std::filesystem::path& room_directory) {
    return {
        .config = Config::load(persona_directory / "config.toml"),
        .system_prompt = read_prompt(persona_directory / "SYSTEM.md")
            + "\n\n" + read_prompt(room_directory / "USER.md"),
    };
}

} // namespace cha
