#pragma once

#include <ctime>
#include <string>
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

// Session labels and note names share these rules. Validation does not
// normalize or otherwise alter their spelling. field is the public name used
// in each error, such as "Session label" or "Name".
void validate_display_label(std::string_view label, std::string_view field);
void validate_session_label(std::string_view label);
// Returns at most count code points of a valid label, without trailing whitespace.
[[nodiscard]] std::string display_label_prefix(std::string_view label, std::size_t count);

// Local timestamp shared by generated session labels and session IDs.
[[nodiscard]] std::string session_timestamp_name(std::time_t now);

} // namespace cha
