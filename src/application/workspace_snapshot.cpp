#include "application/workspace_snapshot.h"

#include "agents/agent.h"
#include "util/text.h"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>

namespace cha {
namespace {

template<typename Value, typename Name>
void require_unique(const std::vector<Value>& values, Name name, std::string_view entity) {
    std::unordered_set<std::string> seen;
    for (const Value& value : values) {
        const std::string folded = fold_ascii(name(value));
        if (!seen.insert(folded).second) {
            throw std::runtime_error(std::string(entity) + " public name '" + std::string(name(value)) + "' is not unique");
        }
    }
}

template<typename Value, typename Name>
void sort_by_name(std::vector<Value>& values, Name name) {
    std::sort(values.begin(), values.end(), [name](const Value& left, const Value& right) {
        return fold_ascii(name(left)) < fold_ascii(name(right));
    });
}

} // namespace

WorkspaceSnapshot::WorkspaceSnapshot(const Workspace& workspace)
    : personas_(workspace.load_personas()), characters_(workspace.character_definitions()) {
    for (const std::string& key : workspace.forums()) forums_.push_back(workspace.load_forum(key));
    require_unique(forums_, [](const Forum& value) -> const std::string& { return value.display_name; }, "Forum");
    for (const Forum& forum : forums_) {
        if (fold_ascii(forum.display_name) == "entrance") {
            throw std::runtime_error("Forum public name '" + forum.display_name + "' is reserved");
        }
    }
    sort_by_name(personas_, [](const Persona& value) -> const std::string& { return value.display_name; });
    sort_by_name(characters_, [](const CharacterDefinitionMetadata& value) -> const std::string& { return value.display_name; });
    sort_by_name(forums_, [](const Forum& value) -> const std::string& { return value.display_name; });
    for (std::size_t index{}; index < personas_.size(); ++index) {
        persona_index_.emplace(fold_ascii(personas_[index].display_name), index);
    }
    for (std::size_t index{}; index < forums_.size(); ++index) {
        forum_index_.emplace(fold_ascii(forums_[index].display_name), index);
    }
}

const Persona* WorkspaceSnapshot::find_persona(std::string_view public_name) const {
    const std::string folded = fold_ascii(public_name);
    const auto found = persona_index_.find(folded);
    return found == persona_index_.end() ? nullptr : &personas_[found->second];
}

const Forum* WorkspaceSnapshot::find_forum(std::string_view public_name) const {
    const std::string folded = fold_ascii(public_name);
    const auto found = forum_index_.find(folded);
    return found == forum_index_.end() ? nullptr : &forums_[found->second];
}

} // namespace cha
