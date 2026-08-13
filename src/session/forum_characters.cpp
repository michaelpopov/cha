#include "session/forum_characters.h"

#include "agents/character.h"

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
    std::vector<CharacterMetadata> characters,
    bool allow_reserved_names)
    : characters_(std::move(characters)) {
    if (characters_.empty()) {
        throw std::invalid_argument("A forum must contain at least one character");
    }
    std::unordered_set<std::string> ids;
    std::unordered_set<std::string> names;
    for (const CharacterMetadata& character : characters_) {
        validate_character_id(character.id);
        if (allow_reserved_names) validate_character_display_name_syntax(character.display_name);
        else validate_character_display_name(character.display_name);
        if (!ids.insert(character.id).second) {
            throw std::invalid_argument(
                "Forum has duplicate character ID '" + character.id + "'");
        }
        if (!names.insert(fold_ascii(character.display_name)).second) {
            throw std::invalid_argument(
                "Forum has duplicate character name '" + character.display_name + "'");
        }
    }
}

const std::vector<CharacterMetadata>& ForumCharacters::all() const noexcept {
    return characters_;
}

const CharacterMetadata* ForumCharacters::find(std::string_view id) const {
    const auto found = std::find_if(
        characters_.begin(),
        characters_.end(),
        [id](const CharacterMetadata& character) { return character.id == id; });
    return found == characters_.end() ? nullptr : &*found;
}

HandleResolution ForumCharacters::resolve_handle(std::string_view handle) const {
    if (handle.empty()) {
        return {};
    }
    const auto named = [this](std::string_view value) -> const CharacterMetadata* {
        const auto found = std::find_if(
            characters_.begin(),
            characters_.end(),
            [value](const CharacterMetadata& character) {
                return ascii_iequals(character.display_name, value);
            });
        return found == characters_.end() ? nullptr : &*found;
    };
    if (const CharacterMetadata* character = named(handle)) {
        return {HandleMatch::resolved, character, {}};
    }
    const std::string_view trimmed = trim_punctuation(handle);
    if (trimmed != handle) {
        if (const CharacterMetadata* character = named(trimmed)) {
            return {HandleMatch::resolved, character, {}};
        }
    }
    if (trimmed.empty()) {
        return {};
    }
    // A character's ID is a stable ASCII handle, which its display name need not
    // be: names can be spelled differently or written in another script. Display
    // names still win, so this only reaches handles that would otherwise be
    // matched loosely or not at all.
    const auto by_id = std::find_if(
        characters_.begin(),
        characters_.end(),
        [trimmed](const CharacterMetadata& character) {
            return ascii_iequals(character.id, trimmed);
        });
    if (by_id != characters_.end()) {
        return {HandleMatch::resolved, &*by_id, {}};
    }
    std::vector<const CharacterMetadata*> candidates;
    for (const CharacterMetadata& character : characters_) {
        if (matches_name_word(character.display_name, trimmed)) {
            candidates.push_back(&character);
        }
    }
    if (candidates.size() == 1) {
        return {HandleMatch::resolved, candidates.front(), {}};
    }
    if (candidates.size() > 1) {
        return {HandleMatch::ambiguous, nullptr, std::move(candidates)};
    }
    for (const CharacterMetadata& character : characters_) {
        if (starts_with_folded(character.display_name, trimmed)
            || starts_with_name_word(character.display_name, trimmed)) {
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
    for (const CharacterMetadata& character : characters_) {
        if (!result.empty()) {
            result += ", ";
        }
        result += "@" + character.display_name;
    }
    return result;
}

std::string format_handle_resolution_notice(
    std::string_view handle,
    const HandleResolution& resolution,
    const ForumCharacters& characters) {
    if (resolution.match == HandleMatch::unknown) {
        return "Unknown character @" + std::string(handle)
            + ". Characters in this forum: " + characters.handle_list();
    }
    std::string result =
        "Ambiguous character @" + std::string(handle) + ": matches ";
    for (std::size_t index = 0; index < resolution.candidates.size(); ++index) {
        if (index) {
            result += ", ";
        }
        result += "@" + resolution.candidates[index]->display_name;
    }
    return result + ". Type more of the name.";
}

std::string format_duplicate_character_notice(std::string_view display_name) {
    return "Multicast target @" + std::string(display_name) + " is duplicated";
}

std::string format_characters_notice(
    const ForumCharacters& characters,
    const std::vector<ModelBackendInfo>& runtime_info,
    const CharacterId& default_character_id) {
    std::ostringstream result;
    result << "Characters in this forum (" << characters.all().size()
           << "), * marks the default.";
    result << " Any unambiguous prefix works.";
    for (const ModelBackendInfo& backend : runtime_info) {
        result << " | " << (backend.character.id == default_character_id ? "* " : "")
               << "@" << backend.character.display_name << "  " << backend.model << "  "
               << backend.api << "  "
               << (backend.streaming ? "streaming" : "non-streaming");
    }
    return result.str();
}

std::string format_session_information(
    std::size_t entry_count,
    const ForumCharacters& characters,
    const std::vector<ModelBackendInfo>& runtime_info,
    const CharacterId& default_character_id) {
    std::ostringstream text;
    text << "Transcript entries: " << entry_count
         << " | " << format_characters_notice(
             characters, runtime_info, default_character_id);
    return text.str();
}

} // namespace cha
