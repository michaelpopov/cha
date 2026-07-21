#pragma once

#include <string>
#include <string_view>

namespace cha {

void validate_agent_id(std::string_view id);
void validate_agent_name(std::string_view name);
[[nodiscard]] std::string fold_ascii(std::string_view value);

} // namespace cha
