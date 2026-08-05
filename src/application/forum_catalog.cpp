#include "application/forum_catalog.h"

#include "application/builtins.h"
#include "application/workspace_inventory.h"
#include "util/text.h"

#include <algorithm>
#include <stdexcept>

namespace cha {
ForumCatalog::ForumCatalog(const Workspace& workspace, const WorkspaceSnapshot& snapshot, SharedPersonaRoster personas, const WorkspaceInventory& inventory)
    : snapshot_(snapshot),
      entrance_source_(make_entrance_session_source(
          workspace, personas, inventory, welcome_storage_)) {
    workspace_sources_.reserve(snapshot_.forums().size());
    for (const Forum& forum : snapshot_.forums()) {
        workspace_sources_.emplace_back(
            fold_ascii(forum.display_name),
            make_workspace_session_source(workspace, forum, personas));
    }
}

const Forum* ForumCatalog::find(std::string_view public_name) const {
    if (fold_ascii(public_name) == fold_ascii(entrance_name)) return &builtin_entrance();
    return snapshot_.find_forum(public_name);
}

std::vector<std::string> ForumCatalog::custom_names() const {
    std::vector<std::string> result;
    for (const Forum& forum : snapshot_.forums()) result.push_back(forum.display_name);
    return result;
}

std::vector<std::string> ForumCatalog::member_names(std::string_view forum_name) const {
    if (fold_ascii(forum_name) == fold_ascii(entrance_name)) return {std::string(assistant_name)};
    const Forum* forum = snapshot_.find_forum(forum_name);
    if (forum == nullptr) throw std::runtime_error("Unknown forum '" + std::string(forum_name) + "'");
    std::vector<std::string> result;
    result.reserve(forum->character_names.size());
    for (const std::string& key : forum->character_names) {
        const auto character = std::ranges::find(
            snapshot_.characters(), key, &CharacterDefinitionMetadata::id);
        if (character == snapshot_.characters().end()) {
            throw std::runtime_error("Forum public name '" + forum->display_name
                + "' member has no character definition");
        }
        result.push_back(character->display_name);
    }
    std::ranges::sort(result, {}, fold_ascii);
    return result;
}

SessionSource& ForumCatalog::source_for(std::string_view forum_name) const {
    if (fold_ascii(forum_name) == fold_ascii(entrance_name)) return *entrance_source_;
    const std::string folded = fold_ascii(forum_name);
    for (const auto& [name, source] : workspace_sources_) {
        if (name == folded) return *source;
    }
    throw std::runtime_error("Unknown forum '" + std::string(forum_name) + "'");
}
} // namespace cha
