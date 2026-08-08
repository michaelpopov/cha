#include "chat/character.h"

namespace cha {

std::string_view to_string(CharacterFont value) {
    switch (value) {
        case CharacterFont::sans: return "sans";
        case CharacterFont::serif: return "serif";
        case CharacterFont::mono: return "mono";
    }
    return "sans";
}

std::string_view to_string(CharacterSlant value) {
    return value == CharacterSlant::italic ? "italic" : "normal";
}

std::string_view to_string(CharacterWeight value) {
    return value == CharacterWeight::bold ? "bold" : "normal";
}

std::string_view to_string(CharacterScale value) {
    switch (value) {
        case CharacterScale::small: return "small";
        case CharacterScale::normal: return "normal";
        case CharacterScale::large: return "large";
    }
    return "normal";
}

} // namespace cha
