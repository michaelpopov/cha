#pragma once

#include "chat/ids.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

// How one character's words are set on screen. This is deliberately a closed
// vocabulary so workspace data cannot inject arbitrary interface styling.
enum class CharacterFont { sans, serif, mono };
enum class CharacterSlant { normal, italic };
enum class CharacterWeight { light, normal, medium, semibold, bold };
enum class CharacterScale { small, normal, large };
enum class CharacterTextColor { normal, muted, accent };

struct CharacterAppearance {
    CharacterFont font{CharacterFont::sans};
    CharacterSlant style{CharacterSlant::normal};
    CharacterWeight weight{CharacterWeight::normal};
    CharacterScale size{CharacterScale::normal};
    CharacterTextColor text_color{CharacterTextColor::normal};

    bool operator==(const CharacterAppearance&) const = default;
};

std::string_view to_string(CharacterFont value);
std::string_view to_string(CharacterSlant value);
std::string_view to_string(CharacterWeight value);
std::string_view to_string(CharacterScale value);
std::string_view to_string(CharacterTextColor value);

// Public, discovery-safe character data. It contains no endpoint, credential,
// model, or other model-provider detail.
struct CharacterMetadata {
    CharacterId id;
    std::string display_name;
    std::optional<std::string> description;
    std::vector<std::string> tags;
    CharacterAppearance appearance;

    bool operator==(const CharacterMetadata&) const = default;
};

} // namespace cha
