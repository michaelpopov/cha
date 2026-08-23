#include "characters/character.h"

#include "chat/transcript.h"
#include "util/public_name.h"
#include "util/text.h"

#include <stdexcept>

namespace cha {

std::vector<SharedCharacterDefinition> share_character_definitions(
    std::vector<CharacterDefinition> definitions) {
    std::vector<SharedCharacterDefinition> shared;
    shared.reserve(definitions.size());
    for (CharacterDefinition& definition : definitions) {
        shared.push_back(std::make_shared<const CharacterDefinition>(
            std::move(definition)));
    }
    return shared;
}

CharacterRuntimeInfo character_runtime_info(
    const CharacterDefinition& definition) {
    return {
        .id = definition.character.id,
        .model = definition.provider.config.model,
        .api = provider_endpoint(definition.provider.config),
        .streaming = definition.provider.config.stream,
    };
}

void validate_character_id(std::string_view id) {
    if (id.empty()) {
        throw std::invalid_argument("Character ID cannot be empty");
    }
    if (id == null_agent_handle) {
        throw std::invalid_argument(
            "Character ID '-' is reserved for the null agent");
    }
    for (const unsigned char character : id) {
        const bool ascii_letter = (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z');
        const bool digit = character >= '0' && character <= '9';
        if (!ascii_letter && !digit && character != '_'
            && character != '-') {
            throw std::invalid_argument(
                "Character ID must contain only ASCII letters, digits, "
                "underscores, and hyphens");
        }
    }
}

void validate_character_display_name_syntax(std::string_view display_name) {
    try {
        validate_public_name(
            display_name, "Character display name", "character.toml", true);
    } catch (const std::runtime_error& error) {
        throw std::invalid_argument(error.what());
    }
    if (display_name == null_agent_name) {
        throw std::invalid_argument(
            "Character display name '-' is reserved for the null agent");
    }
}

void validate_character_display_name(std::string_view display_name) {
    validate_character_display_name_syntax(display_name);
    const std::string folded = fold_ascii(display_name);
    for (const std::string_view reserved : reserved_participant_names) {
        if (folded == reserved) {
            throw std::invalid_argument(
                "Character display name '" + std::string(display_name)
                + "' is reserved");
        }
    }
}

} // namespace cha
