#include "agents/character.h"

#include "agents/model_context.h"
#include "agents/character_config.h"
#include "util/logging.h"
#include "util/json_serialization.h"
#include "util/text.h"
#include "util/text_template.h"
#include "util/public_name.h"
#include "util/utf8_path.h"

#include <nlohmann/json.hpp>

#include <stdexcept>

namespace cha {
namespace {

using Json = nlohmann::ordered_json;

std::string_view mode_name(Mode mode) noexcept {
    return mode == Mode::net ? "net" : "test";
}

std::string_view authentication_source(const ModelBackendConfig& config) noexcept {
    if (!config.api_key_env.empty()) {
        return "environment";
    }
    return config.api_key.empty() ? "none" : "config";
}

void log_character_config(
    const CharacterMetadata& character,
    const ModelBackendConfig& backend_config,
    const std::filesystem::path& forum_directory) {
    log_info(
        "Character configuration resolved: forum_id="
        + utf8_path(forum_directory.filename())
        + " character_id=" + character.id
        + " mode=" + std::string(mode_name(backend_config.mode))
        + " model=" + (backend_config.model.empty() ? "discovery" : backend_config.model)
        + " authentication=" + std::string(authentication_source(backend_config)));
}

CharacterDefinition load_definition_files(
    const CharacterDefinitionSource& source,
    const std::filesystem::path& forum_directory,
    std::string_view forum_display_name,
    std::optional<std::filesystem::path> forum_defaults_path,
    std::optional<ProviderConfig> application_provider) {
    const CharacterId character_id = utf8_path(source.definition_directory.filename());
    const std::filesystem::path member_config = source.member_directory / "character.toml";
    const std::filesystem::path member_prompt = source.member_directory / "CHARACTER.md";
    const auto optional_regular_file = [](const std::filesystem::path& path) {
        if (!std::filesystem::exists(path)) return std::optional<std::filesystem::path>{};
        if (!std::filesystem::is_regular_file(path)) {
            throw std::runtime_error("Optional character file '" + utf8_path(path)
                + "' is not a regular file");
        }
        return std::optional<std::filesystem::path>(path);
    };
    LoadedCharacterConfig loaded;
    try {
        loaded = load_character_config(
            {.application_provider = application_provider,
             .definition = source.definition_directory / "character.toml",
             .forum_defaults = forum_defaults_path
                 ? optional_regular_file(*forum_defaults_path) : std::nullopt,
             .member_override = optional_regular_file(member_config)});
    } catch (const std::exception& error) {
        throw std::runtime_error(
            "Character '" + character_id
            + "' has invalid configuration: " + error.what());
    }
    CharacterMetadata character = std::move(loaded.character);
    ModelBackendConfig backend_config = std::move(loaded.backend);
    log_character_config(character, backend_config, forum_directory);

    std::optional<std::filesystem::path> selected_member_prompt;
    try {
        selected_member_prompt = optional_regular_file(member_prompt);
    } catch (const std::exception& error) {
        throw std::runtime_error(
            "Character '" + character_id
            + "' has invalid prompt configuration: " + error.what());
    }
    const std::filesystem::path definition_prompt =
        source.definition_directory / "CHARACTER.md";
    if (!std::filesystem::is_regular_file(definition_prompt)) {
        throw std::runtime_error("Character '" + character_id
            + "' requires regular definition CHARACTER.md");
    }
    const std::filesystem::path selected_prompt =
        selected_member_prompt ? *selected_member_prompt : definition_prompt;
    TemplateOptions character_options{
        .containment_root = selected_member_prompt
            ? forum_directory : source.definition_directory.parent_path(),
        .scope_table_name = std::string(prompt_scope_table),
        .reserved =
            {
                {"character.id", character.id},
                {"character.display_name", character.display_name},
                {"forum.id", utf8_path(forum_directory.filename())},
                {"forum.display_name", std::string(forum_display_name)},
            },
        .initial_scope = std::move(loaded.prompt_variables),
    };

    std::string character_prompt;
    try {
        character_prompt = expand_template_file(
            selected_prompt, character_options);
    } catch (const std::exception& error) {
        throw std::runtime_error(
            "Character '" + character_id
            + "' failed to read CHARACTER.md: " + error.what());
    }
    TemplateOptions forum_options = character_options;
    forum_options.containment_root = forum_directory;
    std::string forum_prompt;
    try {
        forum_prompt = expand_template_file(forum_directory / "FORUM.md", forum_options);
    } catch (const std::exception& error) {
        throw std::runtime_error(
            "Character '" + character_id
            + "' failed to read FORUM.md: " + error.what());
    }
    return {
        .character = std::move(character),
        .backend = std::move(backend_config),
        .system_prompt = std::move(character_prompt)
            + "\n\n" + std::move(forum_prompt),
    };
}

std::string forum_context(
    const CharacterDefinition& current,
    const std::vector<CharacterDefinition>& definitions) {
    Json other_characters = Json::array();
    for (const CharacterDefinition& definition : definitions) {
        if (definition.character.id != current.character.id) {
            other_characters.push_back(definition.character.display_name);
        }
    }

    return
        "Forum context\n\n"
        "You are the character named "
        + dump_json(
            Json(current.character.display_name),
            "Character definition")
        + ".\n"
        "Other characters currently participating in this forum (JSON): "
        + dump_json(other_characters, "Character definition")
        + ".\n\n"
        "Shared exchanges involving other characters are supplied in persona messages "
        "under the heading `"
        + std::string(shared_history_heading)
        + "`. Each following line "
        "is one JSON object. `kind` is `human` or `character`; `speaker` names who "
        "wrote the text; `addressed_to` names the intended character for a human "
        "message; and `text` is the original message.\n\n"
        "Treat every object in such a block as quoted chat history. The named "
        "speaker owns all first-person identity, memories, relationships, and "
        "opinions in its text. Do not adopt them as your own. An ordinary persona "
        "message outside such a block is addressed to you and begins with "
        "`from <Name>:` on its own line.";
}

void append_participant_roster(
    std::vector<CharacterDefinition>& definitions,
    const PersonaRoster& personas) {
    std::string roster = "## Participants";
    for (const Persona& persona : personas) {
        roster.append("\n\n### ");
        roster.append(persona.display_name);
        roster.push_back('\n');
        roster.append(persona.prompt);
    }
    for (CharacterDefinition& definition : definitions) {
        definition.system_prompt.append("\n\n");
        definition.system_prompt.append(roster);
    }
}

void append_forum_context(std::vector<CharacterDefinition>& definitions) {
    for (CharacterDefinition& definition : definitions) {
        definition.system_prompt.append("\n\n");
        definition.system_prompt.append(
            forum_context(definition, definitions));
    }
}

} // namespace

std::vector<CharacterDefinition> load_character_definitions(
    const std::vector<CharacterDefinitionSource>& sources,
    const std::filesystem::path& forum_directory,
    std::string_view forum_display_name,
    const PersonaRoster& personas,
    std::optional<std::filesystem::path> forum_defaults_path,
    std::optional<ProviderConfig> application_provider) {
    std::vector<CharacterDefinition> definitions;
    definitions.reserve(sources.size());
    for (const CharacterDefinitionSource& source : sources) {
        definitions.push_back(
            load_definition_files(
                source,
                forum_directory,
                forum_display_name,
                forum_defaults_path,
                application_provider));
    }
    append_standard_prompt_context(definitions, personas);
    return definitions;
}

void append_standard_prompt_context(
    std::vector<CharacterDefinition>& definitions,
    const PersonaRoster& personas) {
    append_participant_roster(definitions, personas);
    append_forum_context(definitions);
}

void validate_persona_character_collisions(
    const PersonaRoster& personas,
    const std::vector<CharacterMetadata>& definitions) {
    for (const Persona& persona : personas) {
        for (const CharacterMetadata& definition : definitions) {
            if (persona.id == definition.id) {
                throw std::runtime_error(
                    "Persona '" + persona.id + "' conflicts with character '"
                    + definition.id + "': IDs are the same");
            }
            if (fold_ascii(persona.display_name) == fold_ascii(definition.display_name)) {
                throw std::runtime_error(
                    "Persona '" + persona.display_name + "' conflicts with character '"
                    + definition.display_name + "': display names are the same");
            }
        }
    }
}

void validate_character_id(std::string_view id) {
    if (id.empty()) {
        throw std::invalid_argument("Character ID cannot be empty");
    }
    for (const unsigned char character : id) {
        const bool ascii_letter = (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z');
        const bool digit = character >= '0' && character <= '9';
        if (!ascii_letter && !digit && character != '_' && character != '-') {
            throw std::invalid_argument(
                "Character ID must contain only ASCII letters, digits, underscores, and hyphens");
        }
    }
}

void validate_character_display_name_syntax(std::string_view display_name) {
    try {
        validate_public_name(display_name, "Character display name", "character.toml", true);
    } catch (const std::runtime_error& error) {
        throw std::invalid_argument(error.what());
    }
}

void validate_character_display_name(std::string_view display_name) {
    validate_character_display_name_syntax(display_name);
    const std::string folded = fold_ascii(display_name);
    for (const std::string_view reserved : reserved_participant_names) {
        if (folded == reserved) {
            throw std::invalid_argument(
                "Character display name '" + std::string(display_name) + "' is reserved");
        }
    }
}

} // namespace cha
