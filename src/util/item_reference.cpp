#include "util/item_reference.h"
#include "util/text.h"

namespace cha {

std::optional<std::int64_t> parse_item_reference(std::string_view input) {
    input = trim_view(input);
    if (input.size() != 8 || !input.starts_with("##")) return std::nullopt;
    constexpr std::string_view base62 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    std::int64_t id = 0;
    for (const char digit : input.substr(2)) {
        const auto value = base62.find(digit);
        if (value == std::string_view::npos) return std::nullopt;
        id = id * 62 + static_cast<std::int64_t>(value);
    }
    return id;
}

} // namespace cha
