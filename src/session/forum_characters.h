#pragma once

#include "agents/model_backend.h"
#include "chat/character.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

enum class HandleMatch { resolved, unknown, ambiguous };

// The outcome of resolving a handle typed by the persona.
struct HandleResolution {
    HandleMatch match{HandleMatch::unknown};
    const CharacterMetadata* character{};
    std::vector<const CharacterMetadata*> candidates;
};

[[nodiscard]] std::string format_handle_resolution_notice(
    std::string_view handle,
    const HandleResolution& resolution,
    const class ForumCharacters& characters);
[[nodiscard]] std::string format_duplicate_character_notice(
    std::string_view display_name);
// Runtime details come from immutable character definitions rather than the identity-only
// character view, so both are passed in. The entry count is a plain size for
// the same reason: this wording belongs to the session, not to a transcript.
[[nodiscard]] std::string format_characters_notice(
    const class ForumCharacters& characters,
    const std::vector<ModelBackendInfo>& runtime_info,
    const CharacterId& default_character_id);
[[nodiscard]] std::string format_session_information(
    std::size_t entry_count,
    const class ForumCharacters& characters,
    const std::vector<ModelBackendInfo>& runtime_info,
    const CharacterId& default_character_id);

// The ordered characters participating in one forum. Construction guarantees that the collection is
// non-empty and that character IDs and case-folded names are unique.
class ForumCharacters {
public:
    explicit ForumCharacters(
        std::vector<CharacterMetadata> characters,
        bool allow_reserved_names = false);

    const std::vector<CharacterMetadata>& all() const noexcept;
    const CharacterMetadata* find(std::string_view id) const;
    // Replaces one character's appearance in place for a session-scoped style
    // override; returns false for an unknown ID. Only the appearance field is
    // reachable, so the construction invariants (non-empty, unique IDs, unique
    // folded names, ID/name syntax) all hold by inspection.
    bool set_appearance(std::string_view id, const CharacterAppearance& appearance);
    HandleResolution resolve_handle(std::string_view handle) const;
    std::string handle_list() const;

private:
    std::vector<CharacterMetadata> characters_;
};

} // namespace cha
