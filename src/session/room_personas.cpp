#include "session/room_personas.h"

#include "util/text.h"

#include <algorithm>
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

} // namespace

RoomPersonas::RoomPersonas(std::vector<PersonaInfo> personas)
    : personas_(std::move(personas)) {
    if (personas_.empty()) {
        throw std::invalid_argument("A room must contain at least one persona");
    }
    std::unordered_set<std::string> ids;
    std::unordered_set<std::string> names;
    for (const PersonaInfo& persona : personas_) {
        validate_persona_id(persona.id);
        validate_persona_name(persona.name);
        if (!ids.insert(persona.id).second) {
            throw std::invalid_argument(
                "Room has duplicate persona ID '" + persona.id + "'");
        }
        if (!names.insert(fold_ascii(persona.name)).second) {
            throw std::invalid_argument(
                "Room has duplicate persona name '" + persona.name + "'");
        }
    }
}

const std::vector<PersonaInfo>& RoomPersonas::all() const noexcept {
    return personas_;
}

const PersonaInfo& RoomPersonas::first() const {
    return personas_.front();
}

const PersonaInfo* RoomPersonas::find(std::string_view id) const {
    const auto found = std::find_if(
        personas_.begin(),
        personas_.end(),
        [id](const PersonaInfo& persona) { return persona.id == id; });
    return found == personas_.end() ? nullptr : &*found;
}

HandleResolution RoomPersonas::resolve_handle(std::string_view handle) const {
    if (handle.empty()) {
        return {};
    }
    const auto named = [this](std::string_view value) -> const PersonaInfo* {
        const auto found = std::find_if(
            personas_.begin(),
            personas_.end(),
            [value](const PersonaInfo& persona) {
                return ascii_iequals(persona.name, value);
            });
        return found == personas_.end() ? nullptr : &*found;
    };
    if (const PersonaInfo* persona = named(handle)) {
        return {HandleMatch::resolved, persona, {}};
    }
    const std::string_view trimmed = trim_punctuation(handle);
    if (trimmed != handle) {
        if (const PersonaInfo* persona = named(trimmed)) {
            return {HandleMatch::resolved, persona, {}};
        }
    }
    if (trimmed.empty()) {
        return {};
    }
    std::vector<const PersonaInfo*> candidates;
    for (const PersonaInfo& persona : personas_) {
        if (starts_with_folded(persona.name, trimmed)) {
            candidates.push_back(&persona);
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

std::string RoomPersonas::handle_list() const {
    std::string result;
    for (const PersonaInfo& persona : personas_) {
        if (!result.empty()) {
            result += ", ";
        }
        result += "@" + persona.name;
    }
    return result;
}

} // namespace cha
