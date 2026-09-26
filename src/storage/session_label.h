#pragma once

#include <string_view>

namespace cha {

inline constexpr std::string_view temporary_session_label_prefix = "temp-ts-cha-";

inline bool is_temporary_session_label(std::string_view label) {
    return label.starts_with(temporary_session_label_prefix)
        && label.size() > temporary_session_label_prefix.size()
        && label.substr(temporary_session_label_prefix.size()).find_first_not_of("0123456789")
            == std::string_view::npos;
}

// Session labels are user-authored, single-line display text. Validation does
// not normalize or otherwise alter their spelling.
void validate_session_label(std::string_view label);

} // namespace cha
