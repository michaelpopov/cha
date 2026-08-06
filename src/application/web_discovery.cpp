#include "application/web_discovery.h"

#include "application/builtins.h"
#include "util/text.h"

#include <algorithm>

namespace cha {
namespace {

template<typename Value, typename Name>
void sort_by_name(std::vector<Value>& values, Name name) {
    std::sort(values.begin(), values.end(), [name](const Value& left, const Value& right) {
        return fold_ascii(name(left)) < fold_ascii(name(right));
    });
}

} // namespace

WebDiscovery::WebDiscovery(const Workspace& workspace)
    : workspace_(workspace), personas_(workspace_), inventory_(workspace_) {
    characters_ = workspace_.characters();
    characters_.push_back({std::string(assistant_id), std::string(assistant_name), std::nullopt, {}});
    forums_ = workspace_.forums();
    forums_.push_back(builtin_entrance());
    // The snapshot arrives sorted by display name; the built-ins take their
    // place in that order instead of trailing it.
    sort_by_name(characters_, [](const CharacterDefinitionMetadata& value) -> const std::string& { return value.display_name; });
    sort_by_name(forums_, [](const Forum& value) -> const std::string& { return value.display_name; });
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
