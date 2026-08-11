#include "workspace/workspace_definition.h"

#include "workspace/builtins.h"
#include "agents/character.h"
#include "session/forum_characters.h"
#include "session/not_found_error.h"
#include "util/logging.h"
#include "util/path_name.h"
#include "util/public_name.h"
#include "util/text.h"
#include "util/path_name.h"

#include <nlohmann/json.hpp>
#include <toml++/toml.hpp>

#include <algorithm>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace cha {
namespace {

using Json = nlohmann::ordered_json;

// A custom forum while it is still being loaded. It keeps the directory only
// long enough to read prompts and derive the sessions path; the published
// ForumInfo stays path-free.
struct LoadedForum {
    ForumInfo info;
    std::filesystem::path directory;
};

enum class SubdirectoryNameKind { path_component, url_identifier };

std::vector<std::string> subdirectory_names(
    const std::filesystem::path& directory,
    SubdirectoryNameKind kind) {
    std::vector<std::string> result;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(directory)) {
        if (!entry.is_directory()) {
            continue;
        }
        const std::string name = entry.path().filename().string();
        if (kind == SubdirectoryNameKind::url_identifier) {
            if (!is_url_safe_identifier(name)) {
                log_warn(
                    "Invalid forum directory ignored: path="
                    + utf8_path(entry.path()));
                continue;
            }
        } else {
            require_path_component(name, directory);
        }
        result.push_back(name);
    }
    std::sort(result.begin(), result.end());
    return result;
}

std::string read_text_file(
    const std::filesystem::path& path,
    std::string_view description) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            "Failed to read " + std::string(description) + " '"
            + utf8_path(path) + "'");
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        throw std::runtime_error(
            "Failed to read " + std::string(description) + " '"
            + utf8_path(path) + "'");
    }
    return std::move(contents).str();
}

LoadedForum load_forum_metadata(
    const std::filesystem::path& directory,
    std::string name) {
    const std::filesystem::path path = directory / "config.toml";
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error(
            "Failed to read forum config '" + utf8_path(path) + "'");
    }
    const toml::table table = toml::parse(file, utf8_path(path));
    // Every optional key here silently falls back to a default, so a misspelled
    // one would be indistinguishable from an omitted one without this.
    static constexpr std::string_view fields[]{
        "display_name", "description", "default_character", "default_agent",
        "default_persona"};
    for (const auto& [key, value] : table) {
        (void)value;
        if (std::ranges::find(fields, key.str()) == std::end(fields)) {
            throw std::runtime_error("Forum config '" + utf8_path(path)
                + "' has unknown field '" + std::string(key.str()) + "'");
        }
    }
    const std::optional<std::string> display_name =
        table["display_name"].value<std::string>();
    if (!display_name) {
        throw std::runtime_error(
            "Forum config '" + utf8_path(path)
            + "' requires a non-empty string 'display_name'");
    }
    validate_public_name(*display_name, "Forum name", path);
    const std::optional<std::string> description = table["description"].value<std::string>();
    if (table.contains("description") && !description) {
        throw std::runtime_error("Forum config '" + utf8_path(path) + "' requires string 'description'");
    }
    if (description) validate_description(*description, "Forum", path);
    const std::filesystem::path members_directory = directory / "members";
    if (!std::filesystem::is_directory(members_directory)) {
        throw std::runtime_error(
            "Forum '" + name + "' requires a members/ directory at '"
            + utf8_path(members_directory) + "'");
    }
    const std::vector<std::string> member_ids = subdirectory_names(
        members_directory, SubdirectoryNameKind::path_component);
    for (const std::string& member_id : member_ids) {
        try {
            validate_character_id(member_id);
        } catch (const std::exception& error) {
            throw std::runtime_error(
                "Forum '" + name + "' member ID '" + member_id
                + "' is invalid: " + error.what());
        }
    }
    if (member_ids.empty()) {
        throw std::runtime_error("Members directory '" + utf8_path(members_directory)
            + "' does not contain an entry");
    }
    CharacterId default_character_id;
    const bool has_default_character = table.contains("default_character");
    const bool has_legacy_default_agent = table.contains("default_agent");
    if (has_default_character && has_legacy_default_agent) {
        throw std::runtime_error("Forum config '" + utf8_path(path)
            + "' cannot define both 'default_character' and legacy 'default_agent'");
    }
    if (has_default_character || has_legacy_default_agent) {
        const std::string_view key = has_default_character
            ? "default_character" : "default_agent";
        const std::optional<std::string> configured = table[key].value<std::string>();
        if (!configured || configured->empty()) {
            throw std::runtime_error("Forum config '" + utf8_path(path)
                + "' requires a non-empty string '" + std::string(key) + "'");
        }
        if (!std::ranges::binary_search(member_ids, *configured)) {
            throw std::runtime_error("Forum config '" + utf8_path(path)
                + "' " + std::string(key) + " '" + *configured
                + "' is not a forum member");
        }
        default_character_id = *configured;
    } else {
        default_character_id = member_ids.front();
    }
    std::string default_persona_id = std::string(guest_id);
    if (table.contains("default_persona")) {
        const std::optional<std::string> configured =
            table["default_persona"].value<std::string>();
        if (!configured || configured->empty()) {
            throw std::runtime_error("Forum config '" + utf8_path(path)
                + "' requires a non-empty string 'default_persona'");
        }
        default_persona_id = *configured;
    }
    return {
        .info = {
            .id = std::move(name),
            .display_name = *display_name,
            .description = description,
            .member_ids = member_ids,
            .default_character_id = std::move(default_character_id),
            .default_persona_id = std::move(default_persona_id),
        },
        .directory = directory,
    };
}

std::vector<CharacterMetadata> load_definition_metadata(
    const std::filesystem::path& definitions_directory) {
    if (!std::filesystem::is_directory(definitions_directory)) {
        throw std::runtime_error("Workspace '" + utf8_path(definitions_directory.parent_path())
            + "' requires a characters/ directory");
    }
    std::vector<CharacterMetadata> definitions;
    for (const std::string& id : subdirectory_names(
             definitions_directory, SubdirectoryNameKind::path_component)) {
        try {
            validate_character_id(id);
            const std::filesystem::path directory = definitions_directory / path_from_utf8(id);
            const std::filesystem::path prompt = directory / "CHARACTER.md";
            if (!std::filesystem::is_regular_file(prompt)) {
                throw std::runtime_error("Character '" + id
                    + "' requires regular definition CHARACTER.md");
            }
            definitions.push_back(load_character_metadata(directory / "character.toml"));
        } catch (const std::exception& error) {
            throw std::runtime_error("Character '" + id + "' has invalid definition: " + error.what());
        }
    }
    std::unordered_map<std::string, std::string> display_names;
    for (const CharacterMetadata& definition : definitions) {
        try {
            validate_character_display_name(definition.display_name);
        } catch (const std::exception& error) {
            throw std::runtime_error("Character '" + definition.id + "' has invalid definition: " + error.what());
        }
        const auto [existing, inserted] = display_names.emplace(
            fold_ascii(definition.display_name), definition.id);
        if (!inserted) {
            throw std::runtime_error("Character public name '" + definition.display_name
                + "' is not unique");
        }
    }
    return definitions;
}

bool is_persona_id(std::string_view id) {
    if (id.empty()) return false;
    const auto is_letter = [](unsigned char character) {
        return (character >= 'A' && character <= 'Z')
            || (character >= 'a' && character <= 'z');
    };
    const unsigned char first = static_cast<unsigned char>(id.front());
    if (!is_letter(first) && first != '_') return false;
    for (const char value : id) {
        const unsigned char character = static_cast<unsigned char>(value);
        if (!is_letter(character)
            && !(character >= '0' && character <= '9')
            && character != '_') {
            return false;
        }
    }
    return true;
}

bool is_reserved_participant_name(std::string_view name) {
    const std::string folded = fold_ascii(name);
    return std::ranges::any_of(
        reserved_participant_names,
        [&folded](std::string_view reserved) { return folded == reserved; });
}

void validate_persona_id(std::string_view id, const std::filesystem::path& directory) {
    if (!is_persona_id(id)) {
        throw std::runtime_error(
            "Persona ID '" + std::string(id) + "' in '" + utf8_path(directory)
            + "' must match [A-Za-z_][A-Za-z0-9_]*");
    }
    if (is_reserved_participant_name(id)) {
        throw std::runtime_error(
            "Persona ID '" + std::string(id) + "' in '" + utf8_path(directory)
            + "' is reserved");
    }
}

void validate_persona_display_name(
    std::string_view name,
    const std::filesystem::path& path) {
    validate_public_name(name, "Persona name", path, true);
    if (is_reserved_participant_name(name)) {
        throw std::runtime_error(
            "Persona display name '" + std::string(name) + "' is reserved");
    }
}

Persona load_persona(const std::filesystem::path& directory) {
    const std::string id = utf8_path(directory.filename());
    validate_persona_id(id, directory.parent_path());
    const std::filesystem::path config_path = directory / "persona.toml";
    std::ifstream config_file(config_path, std::ios::binary);
    if (!config_file) {
        throw std::runtime_error(
            "Failed to read persona config '" + utf8_path(config_path) + "'");
    }
    const toml::table table = toml::parse(config_file, utf8_path(config_path));
    for (const auto& [key, value] : table) {
        (void)value;
        if (key.str() != "display_name" && key.str() != "description") {
            throw std::runtime_error(
                "Persona config '" + utf8_path(config_path)
                + "' has unknown field '" + std::string(key.str()) + "'");
        }
    }
    const std::optional<std::string> display_name =
        table["display_name"].value<std::string>();
    validate_persona_display_name(display_name.value_or(""), config_path);

    std::string prompt;
    const std::filesystem::path prompt_path = directory / "PERSONA.md";
    if (std::filesystem::is_regular_file(prompt_path)) {
        prompt = read_text_file(prompt_path, "persona prompt");
    } else if (std::filesystem::exists(prompt_path)) {
        throw std::runtime_error(
            "Persona prompt '" + utf8_path(prompt_path) + "' is not a regular file");
    }
    const std::optional<std::string> description = table["description"].value<std::string>();
    if (table.contains("description") && !description) {
        throw std::runtime_error("Persona config '" + utf8_path(config_path) + "' requires string 'description'");
    }
    if (description) validate_description(*description, "Persona", config_path);
    return {id, *display_name, std::move(prompt), description};
}

PersonaRoster load_personas(const std::filesystem::path& root) {
    const std::filesystem::path personas_directory = root / "personas";
    if (!std::filesystem::is_directory(personas_directory)) {
        throw std::runtime_error(
            "Personas directory '" + utf8_path(personas_directory)
            + "' does not exist; create personas/<id>/persona.toml");
    }
    PersonaRoster personas;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(personas_directory)) {
        if (entry.is_directory()) personas.push_back(load_persona(entry.path()));
    }
    std::sort(personas.begin(), personas.end(), [](const Persona& left, const Persona& right) {
        return left.id < right.id;
    });
    std::unordered_map<std::string, std::string> display_names;
    for (const Persona& persona : personas) {
        const std::string folded = fold_ascii(persona.display_name);
        const auto [existing, inserted] = display_names.emplace(folded, persona.id);
        if (!inserted) {
            throw std::runtime_error(
                "Persona public name '" + persona.display_name + "' is not unique");
        }
    }
    return personas;
}

bool is_builtin_id(std::string_view id) {
    return id == guest_id || id == assistant_id
        || id == entrance_id || id == welcome_id;
}

template<typename Value, typename Name>
void sort_by_name(std::vector<Value>& values, Name name) {
    std::sort(values.begin(), values.end(), [name](const Value& left, const Value& right) {
        return fold_ascii(name(left)) < fold_ascii(name(right));
    });
}

const std::string& character_display_name(const CharacterMetadata& value) {
    return value.display_name;
}

const std::string& forum_display_name(const ForumInfo& value) {
    return value.display_name;
}

std::vector<CharacterDefinition> load_forum_definitions(
    const LoadedForum& forum,
    const PersonaRoster& personas,
    const std::filesystem::path& definitions_directory,
    const ProviderConfig& application_provider) {
    log_info(
        "Loading forum character definitions: forum_id=" + forum.info.id
        + " characters=" + std::to_string(forum.info.member_ids.size()));
    std::vector<CharacterDefinitionSource> sources;
    sources.reserve(forum.info.member_ids.size());
    for (const std::string& member_id : forum.info.member_ids) {
        sources.push_back({
            .definition_directory = definitions_directory / path_from_utf8(member_id),
            .member_directory = forum.directory / "members" / path_from_utf8(member_id),
        });
    }
    const std::filesystem::path defaults_candidate =
        forum.directory / "members" / "character_defaults.toml";
    const std::optional<std::filesystem::path> base_config =
        std::filesystem::exists(defaults_candidate)
        ? std::optional<std::filesystem::path>(defaults_candidate)
        : std::nullopt;
    std::vector<CharacterDefinition> definitions = load_character_definitions(
        sources,
        forum.directory,
        forum.info.display_name,
        personas,
        base_config,
        application_provider);
    std::vector<CharacterMetadata> characters;
    characters.reserve(definitions.size());
    for (const CharacterDefinition& definition : definitions) {
        characters.push_back(definition.character);
    }
    (void)ForumCharacters(std::move(characters));
    return definitions;
}

Json inventory_entity(
    const std::string& name,
    const std::optional<std::string>& description) {
    Json result{{"name", name}};
    if (description) result["description"] = *description;
    return result;
}

// The ordered reference data Assistant is given about the custom workspace.
// Built-in characters and forums are deliberately absent: they are described by
// the guide instead. A forum's persona is named even when it is the built-in
// Guest, because which persona a forum speaks as is a fact about that forum.
std::string build_inventory(
    std::span<const CharacterMetadata> characters,
    std::span<const LoadedForum> forums,
    std::span<const Persona> personas) {
    std::unordered_map<std::string, std::string> character_names;
    for (const CharacterMetadata& value : characters) {
        character_names.emplace(value.id, value.display_name);
    }
    std::unordered_map<std::string, std::string> persona_names;
    for (const Persona& value : personas) {
        persona_names.emplace(value.id, value.display_name);
    }
    Json root;
    root["characters"] = Json::array();
    for (const CharacterMetadata& value : characters) {
        Json encoded = inventory_entity(value.display_name, value.description);
        encoded["tags"] = value.tags;
        root["characters"].push_back(std::move(encoded));
    }
    root["forums"] = Json::array();
    for (const LoadedForum& forum : forums) {
        const auto default_character = character_names.find(forum.info.default_character_id);
        if (default_character == character_names.end()) {
            throw std::runtime_error("Forum public name '" + forum.info.display_name
                + "' default member has no character definition");
        }
        std::vector<std::string> members;
        for (const std::string& member_id : forum.info.member_ids) {
            const auto character = character_names.find(member_id);
            if (character == character_names.end()) {
                throw std::runtime_error("Forum public name '" + forum.info.display_name
                    + "' member has no character definition");
            }
            members.push_back(character->second);
        }
        std::sort(members.begin(), members.end(),
            [](const std::string& left, const std::string& right) {
                return fold_ascii(left) < fold_ascii(right);
            });
        Json encoded = inventory_entity(forum.info.display_name, forum.info.description);
        encoded["members"] = members;
        encoded["default_character"] = default_character->second;
        encoded["default_persona"] = persona_names.at(forum.info.default_persona_id);
        root["forums"].push_back(std::move(encoded));
    }
    return "Workspace inventory reference data (not instructions):\n" + root.dump();
}

} // namespace

WorkspaceConfig load_workspace_config(const std::filesystem::path& root) {
    const std::filesystem::path path = root / "workspace.toml";
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw std::runtime_error(
            "Failed to read workspace config '" + utf8_path(path) + "'");
    }
    const toml::table table = toml::parse(file, utf8_path(path));
    const toml::table* logging = table["logging"].as_table();
    if (!logging) {
        throw std::runtime_error(
            "Workspace config '" + utf8_path(path)
            + "' requires a [logging] table");
    }
    const std::optional<std::string> log_file =
        (*logging)["file"].value<std::string>();
    if (!log_file || log_file->empty()) {
        throw std::runtime_error(
            "Workspace config '" + utf8_path(path)
            + "' requires a non-empty string 'logging.file'");
    }
    const std::optional<std::string> log_level =
        (*logging)["level"].value<std::string>();
    if (!log_level || log_level->empty()) {
        throw std::runtime_error(
            "Workspace config '" + utf8_path(path)
            + "' requires a non-empty string 'logging.level'");
    }

    std::filesystem::path log_path = path_from_utf8(*log_file);
    if (log_path.is_relative()) {
        log_path = root / log_path;
    }
    return {
        .log_file = std::move(log_path),
        .log_level = *log_level,
        .provider = load_provider_config(table, path),
    };
}

WorkspaceDefinition WorkspaceDefinition::load(
    std::filesystem::path root,
    WorkspaceConfig config) {
    WorkspaceDefinition model;
    model.config_ = std::move(config);

    if (!std::filesystem::is_directory(root / "forums")) {
        throw std::runtime_error(
            "Workspace '" + utf8_path(root)
            + "' requires a forums/ directory");
    }
    const std::filesystem::path definitions_directory = root / "characters";
    std::vector<CharacterMetadata> characters =
        load_definition_metadata(definitions_directory);
    const PersonaRoster custom_personas = load_personas(root);
    validate_persona_character_collisions(custom_personas, characters);

    std::unordered_set<std::string> character_ids;
    for (const CharacterMetadata& character : characters) {
        if (is_builtin_id(character.id)) {
            throw std::runtime_error("Character ID '" + character.id + "' is reserved");
        }
        character_ids.insert(character.id);
    }

    // The effective roster a forum may default to is the custom personas plus
    // the built-in Guest, which is also the fallback when it names none.
    std::unordered_set<std::string> persona_ids{std::string(guest_id)};
    for (const Persona& persona : custom_personas) persona_ids.insert(persona.id);

    std::vector<LoadedForum> forums;
    std::unordered_set<std::string> forum_names;
    for (const std::string& id : subdirectory_names(
             root / "forums", SubdirectoryNameKind::url_identifier)) {
        LoadedForum forum = load_forum_metadata(root / "forums" / path_from_utf8(id), id);
        if (is_builtin_id(forum.info.id)) {
            throw std::runtime_error("Forum ID '" + forum.info.id + "' is reserved");
        }
        const std::string folded_name = fold_ascii(forum.info.display_name);
        if (!forum_names.insert(folded_name).second) {
            throw std::runtime_error(
                "Forum public name '" + forum.info.display_name + "' is not unique");
        }
        if (folded_name == fold_ascii(entrance_name)) {
            throw std::runtime_error(
                "Forum public name '" + forum.info.display_name + "' is reserved");
        }
        for (const std::string& member_id : forum.info.member_ids) {
            if (!character_ids.contains(member_id)) {
                throw std::runtime_error("Forum '" + forum.info.id + "' member '" + member_id
                    + "' has no matching character definition");
            }
        }
        if (!persona_ids.contains(forum.info.default_persona_id)) {
            throw std::runtime_error("Forum config '"
                + utf8_path(forum.directory / "config.toml")
                + "' default_persona '" + forum.info.default_persona_id
                + "' does not name a workspace persona");
        }
        forums.push_back(std::move(forum));
    }

    PersonaRoster effective = custom_personas;
    std::sort(effective.begin(), effective.end(), [](const Persona& left, const Persona& right) {
        return fold_ascii(left.display_name) < fold_ascii(right.display_name);
    });
    effective.insert(effective.begin(), builtin_guest());
    model.personas_ = std::make_shared<const PersonaRoster>(std::move(effective));

    sort_by_name(characters, character_display_name);
    sort_by_name(forums, [](const LoadedForum& value) -> const std::string& {
        return value.info.display_name;
    });
    const std::string inventory =
        build_inventory(characters, forums, *model.personas_);

    // Every configured forum is resolved now, so an invalid member override or
    // prompt cannot wait until someone opens that forum to be discovered.
    for (const LoadedForum& forum : forums) {
        // The default persona was checked against the effective roster above,
        // so this forum's sole participant is known to resolve.
        const Persona& persona = *model.find_persona(forum.info.default_persona_id);
        try {
            std::vector<CharacterDefinition> definitions = load_forum_definitions(
                forum, PersonaRoster{persona}, definitions_directory, model.config_.provider);
            for (const CharacterDefinition& definition : definitions) {
                // A character may participate in multiple forums. The detail
                // endpoint is workspace-wide, so retain the first effective
                // character prompt, which is exactly what that definition's
                // agent receives before forum context is appended.
                model.character_markdown_.emplace(
                    definition.character.id, definition.character_prompt);
            }
            model.definitions_.emplace(forum.info.id, std::move(definitions));
        } catch (const std::exception& error) {
            throw std::runtime_error(
                "Forum '" + forum.info.id + "' at '" + utf8_path(forum.directory)
                + "' has invalid character definitions: " + error.what());
        }
        model.session_directories_.push_back(
            {forum.info.id, forum.directory / "sessions"});
        model.forums_.push_back(forum.info);
    }

    // Keep unassigned characters discoverable. They have no effective forum
    // prompt, so there is nothing to expand against until they are assigned.
    for (const CharacterMetadata& character : characters) {
        if (!model.character_markdown_.contains(character.id)) {
            model.character_markdown_.emplace(
                character.id,
                read_text_file(
                    definitions_directory / path_from_utf8(character.id) / "CHARACTER.md",
                    "character definition"));
        }
    }

    characters.push_back(
        {std::string(assistant_id), std::string(assistant_name), std::nullopt, {}});
    model.character_markdown_.emplace(
        std::string(assistant_id), std::string(application_guide()));
    model.definitions_.emplace(
        std::string(entrance_id),
        builtin_assistant_definitions(
            model.config_.provider, inventory, PersonaRoster{builtin_guest()}));
    model.forums_.push_back({
        .id = std::string(entrance_id),
        .display_name = std::string(entrance_name),
        .description = std::nullopt,
        .member_ids = {std::string(assistant_id)},
        .default_character_id = std::string(assistant_id),
        .default_persona_id = std::string(guest_id),
    });

    // The built-ins take their place in display order rather than trailing it.
    sort_by_name(characters, character_display_name);
    sort_by_name(model.forums_, forum_display_name);
    model.characters_ = std::move(characters);
    for (std::size_t index{}; index < model.characters_.size(); ++index) {
        model.character_index_.emplace(model.characters_[index].id, index);
    }
    for (std::size_t index{}; index < model.forums_.size(); ++index) {
        model.forum_index_.emplace(model.forums_[index].id, index);
    }
    return model;
}

const CharacterMetadata* WorkspaceDefinition::find_character(
    std::string_view id) const noexcept {
    const auto found = character_index_.find(std::string(id));
    return found == character_index_.end() ? nullptr : &characters_[found->second];
}

const Persona* WorkspaceDefinition::find_persona(std::string_view id) const noexcept {
    const auto found = std::find_if(
        personas_->begin(), personas_->end(),
        [id](const Persona& persona) { return persona.id == id; });
    return found == personas_->end() ? nullptr : &*found;
}

const ForumInfo* WorkspaceDefinition::find_forum(std::string_view id) const noexcept {
    const auto found = forum_index_.find(std::string(id));
    return found == forum_index_.end() ? nullptr : &forums_[found->second];
}

std::string_view WorkspaceDefinition::character_markdown(std::string_view id) const {
    const auto found = character_markdown_.find(std::string(id));
    if (found == character_markdown_.end()) {
        throw std::runtime_error("Character '" + std::string(id) + "' is not defined");
    }
    return found->second;
}

std::vector<ForumSessionDirectory> WorkspaceDefinition::session_directories() const {
    return session_directories_;
}

std::vector<CharacterDefinition> WorkspaceDefinition::copy_definitions_for(
    std::string_view forum_id) const {
    const auto found = definitions_.find(std::string(forum_id));
    if (found == definitions_.end()) {
        throw ForumNotFoundError("Forum '" + std::string(forum_id) + "' does not exist");
    }
    return found->second;
}

} // namespace cha
