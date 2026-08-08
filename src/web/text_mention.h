#pragma once

#include <string>
#include <string_view>

namespace cha::web {

// A prompt split at its leading mention: the handle it addresses, empty when the prompt names no
// character, and the message text that remains after removing it.
struct AddressedPrompt {
    std::string handle;
    std::string text;
};

AddressedPrompt parse_addressed_prompt(std::string_view input);

} // namespace cha::web
