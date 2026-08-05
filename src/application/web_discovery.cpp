#include "application/web_discovery.h"

#include "application/builtins.h"

namespace cha {

WebDiscovery::WebDiscovery(const Workspace& workspace)
    : workspace_(workspace), personas_(workspace_), inventory_(workspace_) {
    characters_ = workspace_.characters();
    characters_.push_back({std::string(assistant_id), std::string(assistant_name), std::nullopt, {}});
    forums_ = workspace_.forums();
    forums_.push_back(builtin_entrance());
}

const Persona* WebDiscovery::find_persona(std::string_view id) const {
    for (const Persona& persona : personas()) {
        if (persona.id == id) return &persona;
    }
    return nullptr;
}

const CharacterDefinitionMetadata* WebDiscovery::find_character(std::string_view id) const {
    for (const CharacterDefinitionMetadata& character : characters_) {
        if (character.id == id) return &character;
    }
    return nullptr;
}

const Forum* WebDiscovery::find_forum(std::string_view id) const {
    for (const Forum& forum : forums_) {
        if (forum.name == id) return &forum;
    }
    return nullptr;
}

} // namespace cha
