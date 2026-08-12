#pragma once

#include <string_view>

namespace cha {

// Session labels are user-authored, single-line display text. Validation does
// not normalize or otherwise alter their spelling.
void validate_session_label(std::string_view label);

} // namespace cha
