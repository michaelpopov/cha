#pragma once

#include <string_view>

namespace cha {

inline constexpr std::string_view temporary_session_label = "New session";

inline bool is_temporary_session_label(std::string_view label) {
    if (label == temporary_session_label) return true;
    // Sessions saved by older versions can still receive an automatic name.
    constexpr std::string_view legacy_prefix = "temp-ts-cha-";
    return label.starts_with(legacy_prefix)
        && label.size() > legacy_prefix.size()
        && label.substr(legacy_prefix.size()).find_first_not_of("0123456789")
            == std::string_view::npos;
}

// Session labels are user-authored, single-line display text. Validation does
// not normalize or otherwise alter their spelling.
void validate_session_label(std::string_view label);

} // namespace cha
