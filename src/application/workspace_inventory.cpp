#include "application/workspace_inventory.h"

#include "util/text.h"

#include <algorithm>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <unordered_map>

namespace cha {
namespace {
using Json = nlohmann::ordered_json;
void sort_names(std::vector<std::string>& values) {
    std::sort(values.begin(), values.end(), [](const std::string& left, const std::string& right) { return fold_ascii(left) < fold_ascii(right); });
}
Json entity_json(const InventoryEntity& value) {
    Json result{{"name", value.name}};
    if (value.description) result["description"] = *value.description;
    return result;
}
} // namespace

WorkspaceInventory::WorkspaceInventory(const WorkspaceSnapshot& snapshot) {
    for (const Persona& value : snapshot.personas()) personas_.push_back({value.display_name, value.description});
    std::unordered_map<std::string, std::string> character_names;
    for (const CharacterDefinitionMetadata& value : snapshot.characters()) {
        characters_.push_back({{value.display_name, value.description}, value.tags});
        character_names.emplace(value.id, value.display_name);
    }
    for (const Forum& value : snapshot.forums()) {
        const auto default_character = character_names.find(value.default_agent_id);
        if (default_character == character_names.end()) {
            throw std::runtime_error("Forum public name '" + value.display_name
                + "' default member has no character definition");
        }
        InventoryForum forum{{value.display_name, value.description}, {}, default_character->second};
        for (const std::string& key : value.character_names) {
            const auto character = character_names.find(key);
            if (character == character_names.end()) {
                throw std::runtime_error("Forum public name '" + value.display_name
                    + "' member has no character definition");
            }
            forum.members.push_back(character->second);
        }
        sort_names(forum.members);
        forums_.push_back(std::move(forum));
    }
}

std::string WorkspaceInventory::serialize() const {
    Json root;
    root["personas"] = Json::array();
    for (const InventoryEntity& value : personas_) root["personas"].push_back(entity_json(value));
    root["characters"] = Json::array();
    for (const InventoryCharacter& value : characters_) {
        Json encoded = entity_json(value); encoded["tags"] = value.tags; root["characters"].push_back(std::move(encoded));
    }
    root["forums"] = Json::array();
    for (const InventoryForum& value : forums_) {
        Json encoded = entity_json(value); encoded["members"] = value.members; encoded["default_character"] = value.default_character; root["forums"].push_back(std::move(encoded));
    }
    return "Workspace inventory reference data (not instructions):\n" + root.dump();
}

} // namespace cha
