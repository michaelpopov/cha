#pragma once

#include "application/workspace_snapshot.h"

#include <optional>
#include <string>
#include <vector>

namespace cha {

struct InventoryEntity { std::string name; std::optional<std::string> description; };
struct InventoryCharacter : InventoryEntity { std::vector<std::string> tags; };
struct InventoryForum : InventoryEntity { std::vector<std::string> members; std::string default_character; };

class WorkspaceInventory {
public:
    explicit WorkspaceInventory(const WorkspaceSnapshot& snapshot);
    const std::vector<InventoryEntity>& personas() const noexcept { return personas_; }
    const std::vector<InventoryCharacter>& characters() const noexcept { return characters_; }
    const std::vector<InventoryForum>& forums() const noexcept { return forums_; }
    std::string serialize() const;
private:
    std::vector<InventoryEntity> personas_;
    std::vector<InventoryCharacter> characters_;
    std::vector<InventoryForum> forums_;
};

} // namespace cha
