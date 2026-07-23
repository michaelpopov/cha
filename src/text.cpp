#include "text.h"

#include <cctype>

namespace cha {

bool is_space(char character) {
    return std::isspace(static_cast<unsigned char>(character)) != 0;
}

std::size_t find_whitespace(std::string_view value) {
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (is_space(value[index])) {
            return index;
        }
    }
    return std::string_view::npos;
}

std::string_view trim_view(std::string_view value) {
    while (!value.empty() && is_space(value.front())) {
        value.remove_prefix(1);
    }
    while (!value.empty() && is_space(value.back())) {
        value.remove_suffix(1);
    }
    return value;
}

std::string fold_ascii(std::string_view value) {
    std::string result(value);
    for (char& character : result) {
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return result;
}

} // namespace cha
