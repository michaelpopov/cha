#pragma once

#include "agents/agent.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

enum class HandleMatch { resolved, unknown, ambiguous };

// The outcome of resolving a handle typed by the persona.
struct HandleResolution {
    HandleMatch match{HandleMatch::unknown};
    const CharacterInfo* character{};
    std::vector<const CharacterInfo*> candidates;
};

[[nodiscard]] std::string format_handle_resolution_notice(
    std::string_view handle,
    const HandleResolution& resolution,
    const class ForumCharacters& characters);
[[nodiscard]] std::string format_duplicate_character_notice(
    std::string_view name);
// Runtime details come from CompletionExecutor rather than the identity-only
// character view, so both are passed in. The entry count is a plain size for
// the same reason: this wording belongs to the session, not to a transcript.
[[nodiscard]] std::string format_characters_notice(
    const class ForumCharacters& characters,
    const std::vector<AgentRuntimeInfo>& runtime_info,
    const ParticipantId& default_agent_id);
[[nodiscard]] std::string format_session_information(
    std::size_t entry_count,
    const class ForumCharacters& characters,
    const std::vector<AgentRuntimeInfo>& runtime_info,
    const ParticipantId& default_agent_id);

// The ordered characters participating in one forum. Construction guarantees that the collection is
// non-empty and that character IDs and case-folded names are unique.
class ForumCharacters {
public:
    explicit ForumCharacters(
        std::vector<CharacterInfo> characters,
        bool allow_reserved_names = false);

    const std::vector<CharacterInfo>& all() const noexcept;
    const CharacterInfo& first() const;
    const CharacterInfo* find(std::string_view id) const;
    HandleResolution resolve_handle(std::string_view handle) const;
    std::string handle_list() const;

private:
    std::vector<CharacterInfo> characters_;
};

} // namespace cha
