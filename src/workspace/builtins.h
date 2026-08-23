#pragma once

#include "workspace/workspace.h"

#include <string_view>

namespace cha {

inline constexpr std::string_view guest_name = "Guest";
inline constexpr std::string_view assistant_name = "Assistant";
inline constexpr std::string_view entrance_name = "Entrance";
inline constexpr std::string_view welcome_name = "Welcome";
inline constexpr std::string_view guest_id = workspace_guest_id;
inline constexpr std::string_view assistant_id = workspace_assistant_id;
inline constexpr std::string_view entrance_id = workspace_entrance_id;
inline constexpr std::string_view welcome_id = "builtin-welcome";

} // namespace cha
