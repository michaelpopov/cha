#pragma once

#include "characters/character_config.h"
#include "chat/character.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

inline constexpr std::string_view reserved_participant_names[] = {
    "persona", "system", "error", "human", "assistant", "agent",
    "character", "you", "guest",
};

// Immutable input retained by one provider request. Production constructs it
// from the current Workspace when generation begins.
struct CharacterDefinition {
    CharacterMetadata character;
    ProviderSelection provider;
    std::string character_prompt;
    std::string character_description;
    std::string system_prompt;
};

struct CharacterRuntimeInfo {
    CharacterId id;
    std::string model;
    std::string api;
    bool streaming{};
};

CharacterRuntimeInfo character_runtime_info(
    const CharacterDefinition& definition);

using SharedCharacterDefinition = std::shared_ptr<const CharacterDefinition>;

std::vector<SharedCharacterDefinition> share_character_definitions(
    std::vector<CharacterDefinition> definitions);

void validate_character_id(std::string_view id);
void validate_character_display_name_syntax(std::string_view display_name);
void validate_character_display_name(std::string_view display_name);

} // namespace cha
