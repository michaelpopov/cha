#pragma once

#include "characters/character_config.h"
#include "chat/character_metadata.h"

#include <memory>
#include <string>
#include <vector>

namespace cha {

// Immutable input retained by one provider request. Production constructs it
// from the current Workspace when generation begins.
struct CharacterDefinition {
    CharacterMetadata character;
    ProviderSelection provider;
    std::string character_prompt;
    std::string character_description;
    std::string system_prompt;
};

using SharedCharacterDefinition = std::shared_ptr<const CharacterDefinition>;

std::vector<SharedCharacterDefinition> share_character_definitions(
    std::vector<CharacterDefinition> definitions);

} // namespace cha
