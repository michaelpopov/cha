#include "characters/character.h"

#include <memory>

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

} // namespace cha
