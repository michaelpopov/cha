#pragma once

#include <string>
#include <string_view>

namespace cha {

void validate_agent_id(std::string_view id);
void validate_agent_name(std::string_view name);

} // namespace cha
