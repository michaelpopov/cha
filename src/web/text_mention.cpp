#include "web/text_mention.h"

#include "util/text.h"

namespace cha::web {

AddressedPrompt parse_addressed_prompt(std::string_view input) {
    std::size_t start = 0;
    while (start < input.size() && is_space(input[start])) {
        ++start;
    }
    const std::string_view trimmed = input.substr(start);
    if (!trimmed.starts_with('@')) {
        return {{}, std::string(input)};
    }
    if (trimmed.starts_with("@@")) {
        std::string text(input);
        text.erase(start, 1);
        return {{}, std::move(text)};
    }
    std::size_t end = 1;
    while (end < trimmed.size() && !is_space(trimmed[end])) {
        ++end;
    }
    if (end == 1) {
        return {{}, std::string(input)};
    }
    std::size_t body = end;
    while (body < trimmed.size() && is_space(trimmed[body])) {
        ++body;
    }
    return {std::string(trimmed.substr(1, end - 1)), std::string(trimmed.substr(body))};
}

} // namespace cha::web
