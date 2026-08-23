#include "support/test_workspace.h"

#include "session/session_database.h"
#include "workspace/workspace.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <string>
#include <utility>

namespace cha::test {
namespace {

std::atomic_uint64_t next_workspace_serial{};

std::string quoted(std::string_view value) {
    return nlohmann::json(value).dump();
}

std::string_view mode_name(Mode value) {
    return value == Mode::net ? "net" : "test";
}

std::string_view reasoning_format_name(ReasoningFormat value) {
    switch (value) {
    case ReasoningFormat::automatic: return "auto";
    case ReasoningFormat::none: return "none";
    case ReasoningFormat::reasoning_content: return "reasoning_content";
    case ReasoningFormat::reasoning: return "reasoning";
    }
    return "auto";
}

std::string_view api_name(ProviderApi value) {
    return value == ProviderApi::responses ? "responses" : "chat_completions";
}

std::string_view web_search_name(WebSearchMode value) {
    switch (value) {
    case WebSearchMode::off: return "off";
    case WebSearchMode::automatic: return "auto";
    case WebSearchMode::required: return "required";
    }
    return "off";
}

std::string_view retention_name(CacheRetention value) {
    switch (value) {
    case CacheRetention::off: return "off";
    case CacheRetention::short_: return "short";
    case CacheRetention::long_: return "long";
    }
    return "short";
}

void write_provider_config(
    const std::filesystem::path& path,
    const ModelBackendConfig& config) {
    std::ofstream file(path);
    file << "host = " << quoted(config.host) << '\n'
         << "port = " << config.port << '\n'
         << "base_path = " << quoted(config.base_path) << '\n'
         << "mode = " << quoted(mode_name(config.mode)) << '\n'
         << "model = " << quoted(config.model) << '\n'
         << "stream = " << (config.stream ? "true" : "false") << '\n';
    if (config.temperature) file << "temperature = " << *config.temperature << '\n';
    if (config.max_tokens) file << "max_tokens = " << *config.max_tokens << '\n';
    file << "timeout_s = " << config.timeout_s << '\n'
         << "idle_timeout_s = " << config.idle_timeout_s << '\n'
         << "api_key_env = " << quoted(config.api_key_env) << '\n'
         << "reasoning_effort = " << quoted(config.reasoning_effort) << '\n'
         << "reasoning_format = " << quoted(reasoning_format_name(config.reasoning_format)) << '\n'
         << "https = " << (config.https ? "true" : "false") << '\n'
         << "api = " << quoted(api_name(config.api)) << '\n'
         << "web_search = " << quoted(web_search_name(config.web_search)) << '\n'
         << "cache_retention = " << quoted(retention_name(config.cache_retention)) << '\n';
}

void write_style_config(
    const std::filesystem::path& path,
    const CharacterAppearance& appearance) {
    std::ofstream(path)
        << "font = " << quoted(to_string(appearance.font)) << '\n'
        << "style = " << quoted(to_string(appearance.style)) << '\n'
        << "weight = " << quoted(to_string(appearance.weight)) << '\n'
        << "size = " << quoted(to_string(appearance.size)) << '\n'
        << "text_color = " << quoted(to_string(appearance.text_color)) << '\n';
}

} // namespace

TestWorkspace::TestWorkspace()
    : root_(
          std::filesystem::temp_directory_path()
          / ("cha_web_workspace_"
             + std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count())
             + "_" + std::to_string(++next_workspace_serial))) {
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

PublishedTestWorkspace publish_test_workspace(
    const std::vector<CharacterDefinition>& definitions,
    const PersonaRoster& personas,
    std::string_view default_character_id,
    const std::filesystem::path& database_path,
    SessionIdentity identity,
    const std::vector<TestWorkspaceStyle>& styles,
    bool reuse_current) {
    if ((identity.forum_id.empty() || identity.session_id.empty())
        && std::filesystem::is_regular_file(database_path)) {
        const SessionDatabaseMetadata metadata =
            read_session_database_metadata(database_path);
        if (identity.forum_id.empty()) identity.forum_id = metadata.forum;
        if (identity.session_id.empty()) identity.session_id = metadata.id;
    }
    if (identity.forum_id.empty()) identity.forum_id = "test-forum";
    if (identity.session_id.empty()) identity.session_id = "test-session";

    if (reuse_current) {
        const std::shared_ptr<const Workspace> current = getws();
        const WorkspaceForum* const forum = current == nullptr
            ? nullptr : current->find_forum(identity.forum_id);
        bool usable = forum != nullptr;
        for (const CharacterDefinition& definition : definitions) {
            const WorkspaceForumMember* const member = current == nullptr
                ? nullptr
                : current->find_forum_member(
                    identity.forum_id, definition.character.id);
            const WorkspaceCharacter* const character = current == nullptr
                ? nullptr : current->find_character(definition.character.id);
            usable = usable && member != nullptr && character != nullptr
                && character->character.display_name
                    == definition.character.display_name;
        }
        for (const Persona& persona : personas) {
            const WorkspacePersona* const configured = current == nullptr
                ? nullptr : current->find_persona(persona.id);
            usable = usable && configured != nullptr
                && configured->display_name == persona.display_name;
        }
        if (usable) {
            return {
                .identity = std::move(identity),
                .default_persona_id = personas.empty()
                    ? std::string(workspace_guest_id) : personas.front().id,
            };
        }
    }

    TestWorkspace workspace;
    std::error_code ignored;
    std::filesystem::remove_all(workspace.root(), ignored);
    const std::filesystem::path root = workspace.root();
    std::filesystem::create_directories(root / "system" / "assistant");
    std::filesystem::create_directories(root / "personas");
    std::filesystem::create_directories(root / "characters");
    const std::filesystem::path forum = root / "forums" / identity.forum_id;
    std::filesystem::create_directories(forum / "members");

    std::ofstream(root / "workspace.toml")
        << "[logging]\nfile = \"cha.log\"\nlevel = \"off\"\n";

    std::vector<std::string> provider_ids;
    provider_ids.reserve(definitions.size());
    for (std::size_t index{}; index < definitions.size(); ++index) {
        const std::string provider_id = "test-provider-" + std::to_string(index);
        provider_ids.push_back(provider_id);
        const std::filesystem::path directory =
            root / "system" / "providers" / provider_id;
        std::filesystem::create_directories(directory);
        write_provider_config(directory / "config.toml", definitions[index].provider.config);
    }
    if (provider_ids.empty()) {
        provider_ids.push_back("test-provider");
        const std::filesystem::path directory =
            root / "system" / "providers" / provider_ids.front();
        std::filesystem::create_directories(directory);
        ModelBackendConfig fallback{
            .host = "127.0.0.1",
            .port = 1,
            .model = "test-model",
            .web_search = WebSearchMode::off,
        };
        write_provider_config(directory / "config.toml", fallback);
    }
    std::ofstream(root / "system" / "assistant" / "character.toml")
        << "display_name = \"Assistant\"\nprovider = "
        << quoted(provider_ids.front()) << '\n';

    for (const TestWorkspaceStyle& style : styles) {
        const std::filesystem::path directory =
            root / "system" / "styles" / style.id;
        std::filesystem::create_directories(directory);
        write_style_config(directory / "config.toml", style.appearance);
    }

    for (std::size_t index{}; index < definitions.size(); ++index) {
        const CharacterDefinition& definition = definitions[index];
        const std::string configured_style =
            "configured-" + std::to_string(index);
        const bool has_configured_style =
            definition.character.appearance != CharacterAppearance{};
        if (has_configured_style) {
            const std::filesystem::path style_directory =
                root / "system" / "styles" / configured_style;
            std::filesystem::create_directories(style_directory);
            write_style_config(
                style_directory / "config.toml", definition.character.appearance);
        }

        const std::filesystem::path character =
            root / "characters" / definition.character.id;
        std::filesystem::create_directories(character);
        std::ofstream config(character / "character.toml");
        config << "display_name = " << quoted(definition.character.display_name) << '\n'
               << "provider = " << quoted(provider_ids[index]) << '\n';
        if (has_configured_style) {
            config << "style = " << quoted(configured_style) << '\n';
        }
        if (definition.character.description) {
            config << "description = " << quoted(*definition.character.description) << '\n';
        }
        if (!definition.character.tags.empty()) {
            config << "tags = [";
            for (std::size_t tag{}; tag < definition.character.tags.size(); ++tag) {
                if (tag) config << ", ";
                config << quoted(definition.character.tags[tag]);
            }
            config << "]\n";
        }
        std::string prompt = definition.character_prompt;
        if (prompt.empty()) prompt = definition.character_description;
        if (prompt.empty()) prompt = definition.system_prompt;
        std::ofstream(character / "CHARACTER.md") << prompt;
        std::filesystem::create_directories(
            forum / "members" / definition.character.id);
    }

    for (const Persona& persona : personas) {
        if (persona.id == workspace_guest_id) continue;
        const std::filesystem::path directory = root / "personas" / persona.id;
        std::filesystem::create_directories(directory);
        std::ofstream config(directory / "persona.toml");
        config << "display_name = " << quoted(persona.display_name) << '\n';
        if (persona.description) {
            config << "description = " << quoted(*persona.description) << '\n';
        }
        if (!persona.prompt.empty()) {
            std::ofstream(directory / "PERSONA.md") << persona.prompt;
        }
    }

    const std::string default_persona_id = personas.empty()
        ? std::string(workspace_guest_id) : personas.front().id;
    std::ofstream(forum / "config.toml")
        << "display_name = \"Test forum\"\n"
        << "default_character = " << quoted(default_character_id) << '\n'
        << "default_persona = " << quoted(default_persona_id) << '\n';
    std::ofstream(forum / "FORUM.md");

    loadws(root);
    return {
        .identity = std::move(identity),
        .default_persona_id = default_persona_id,
    };
}

} // namespace cha::test
