#include "session/forum_characters.h"

#include "util/text.h"

#include <algorithm>
#include <sstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace cha {
namespace {

bool ascii_iequals(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        char left_character = left[index];
        char right_character = right[index];
        if (left_character >= 'A' && left_character <= 'Z') {
            left_character = static_cast<char>(left_character - 'A' + 'a');
        }
        if (right_character >= 'A' && right_character <= 'Z') {
            right_character = static_cast<char>(right_character - 'A' + 'a');
        }
        if (left_character != right_character) {
            return false;
        }
    }
    return true;
}

bool starts_with_folded(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size()
        && ascii_iequals(value.substr(0, prefix.size()), prefix);
}

std::string_view trim_punctuation(std::string_view handle) {
    while (!handle.empty()
           && std::string_view(",.;:!?").find(handle.back())
               != std::string_view::npos) {
        handle.remove_suffix(1);
    }
    return handle;
}

bool matches_name_word(std::string_view name, std::string_view handle) {
    std::size_t start = 0;
    while (start < name.size()) {
        while (start < name.size() && is_space(name[start])) {
            ++start;
        }
        const std::size_t end = start;
        while (start < name.size() && !is_space(name[start])) {
            ++start;
        }
        if (start > end
            && ascii_iequals(name.substr(end, start - end), handle)) {
            return true;
        }
    }
    return false;
}

bool starts_with_name_word(std::string_view name, std::string_view handle) {
    std::size_t start = 0;
    while (start < name.size()) {
        while (start < name.size() && is_space(name[start])) {
            ++start;
        }
        const std::size_t end = start;
        while (start < name.size() && !is_space(name[start])) {
            ++start;
        }
        if (start > end
            && starts_with_folded(name.substr(end, start - end), handle)) {
            return true;
        }
    }
    return false;
}

} // namespace

ForumCharacters::ForumCharacters(
    std::vector<CharacterInfo> characters,
    bool allow_reserved_names)
    : characters_(std::move(characters)) {
    if (characters_.empty()) {
        throw std::invalid_argument("A forum must contain at least one character");
    }
    std::unordered_set<std::string> ids;
    std::unordered_set<std::string> names;
    for (const CharacterInfo& character : characters_) {
        validate_character_id(character.id);
        if (allow_reserved_names) validate_character_name_syntax(character.name);
        else validate_character_name(character.name);
        if (!ids.insert(character.id).second) {
            throw std::invalid_argument(
                "Forum has duplicate character ID '" + character.id + "'");
        }
        if (!names.insert(fold_ascii(character.name)).second) {
            throw std::invalid_argument(
                "Forum has duplicate character name '" + character.name + "'");
        }
    }
}

const std::vector<CharacterInfo>& ForumCharacters::all() const noexcept {
    return characters_;
}

const CharacterInfo& ForumCharacters::first() const {
    return characters_.front();
}

const CharacterInfo* ForumCharacters::find(std::string_view id) const {
    const auto found = std::find_if(
        characters_.begin(),
        characters_.end(),
        [id](const CharacterInfo& character) { return character.id == id; });
    return found == characters_.end() ? nullptr : &*found;
}

HandleResolution ForumCharacters::resolve_handle(std::string_view handle) const {
    if (handle.empty()) {
        return {};
    }
    const auto named = [this](std::string_view value) -> const CharacterInfo* {
        const auto found = std::find_if(
            characters_.begin(),
            characters_.end(),
            [value](const CharacterInfo& character) {
                return ascii_iequals(character.name, value);
            });
        return found == characters_.end() ? nullptr : &*found;
    };
    if (const CharacterInfo* character = named(handle)) {
        return {HandleMatch::resolved, character, {}};
    }
    const std::string_view trimmed = trim_punctuation(handle);
    if (trimmed != handle) {
        if (const CharacterInfo* character = named(trimmed)) {
            return {HandleMatch::resolved, character, {}};
        }
    }
    if (trimmed.empty()) {
        return {};
    }
    std::vector<const CharacterInfo*> candidates;
    for (const CharacterInfo& character : characters_) {
        if (matches_name_word(character.name, trimmed)) {
            candidates.push_back(&character);
        }
    }
    if (candidates.size() == 1) {
        return {HandleMatch::resolved, candidates.front(), {}};
    }
    if (candidates.size() > 1) {
        return {HandleMatch::ambiguous, nullptr, std::move(candidates)};
    }
    for (const CharacterInfo& character : characters_) {
        if (starts_with_folded(character.name, trimmed)
            || starts_with_name_word(character.name, trimmed)) {
            candidates.push_back(&character);
        }
    }
    if (candidates.size() == 1) {
        return {HandleMatch::resolved, candidates.front(), {}};
    }
    if (candidates.empty()) {
        return {};
    }
    return {HandleMatch::ambiguous, nullptr, std::move(candidates)};
}

std::string ForumCharacters::handle_list() const {
    std::string result;
    for (const CharacterInfo& character : characters_) {
        if (!result.empty()) {
            result += ", ";
        }
        result += "@" + character.name;
    }
    return result;
}

std::string format_handle_resolution_notice(
    std::string_view handle,
    const HandleResolution& resolution,
    const ForumCharacters& characters) {
    if (resolution.match == HandleMatch::unknown) {
        return "Unknown agent @" + std::string(handle)
            + ". Characters in this forum: " + characters.handle_list();
    }
    std::string result =
        "Ambiguous agent @" + std::string(handle) + ": matches ";
    for (std::size_t index = 0; index < resolution.candidates.size(); ++index) {
        if (index) {
            result += ", ";
        }
        result += "@" + resolution.candidates[index]->name;
    }
    return result + ". Type more of the name.";
}

std::string format_duplicate_character_notice(std::string_view name) {
    return "Multicast target @" + std::string(name) + " is duplicated";
}

std::string format_characters_notice(
    const ForumCharacters& characters,
    const std::vector<AgentRuntimeInfo>& runtime_info,
    const ParticipantId& default_agent_id) {
    std::ostringstream result;
    result << "Characters in this forum (" << characters.all().size()
           << "), * marks the default.";
    result << " Any unambiguous prefix works.";
    for (const AgentRuntimeInfo& agent : runtime_info) {
        result << " | " << (agent.character.id == default_agent_id ? "* " : "")
               << "@" << agent.character.name << "  " << agent.model << "  "
               << agent.api << "  "
               << (agent.streaming ? "streaming" : "non-streaming");
    }
    return result.str();
}

std::string format_session_information(
    std::size_t entry_count,
    const ForumCharacters& characters,
    const std::vector<AgentRuntimeInfo>& runtime_info,
    const ParticipantId& default_agent_id) {
    std::ostringstream text;
    text << "Transcript entries: " << entry_count
         << " | " << format_characters_notice(
             characters, runtime_info, default_agent_id);
    return text.str();
}

} // namespace cha
