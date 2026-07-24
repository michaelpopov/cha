#pragma once

#include <string>
#include <string_view>

namespace cha {

struct AddressedPrompt {
    std::string handle;
    std::string text;
};

[[nodiscard]] AddressedPrompt parse_addressed_prompt(std::string_view input);

} // namespace cha
