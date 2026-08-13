#include "util/text.h"

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

namespace {

char fold_character(char value) {
    return value >= 'A' && value <= 'Z'
        ? static_cast<char>(value - 'A' + 'a')
        : value;
}

} // namespace

bool ascii_iequals(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (fold_character(left[index]) != fold_character(right[index])) {
            return false;
        }
    }
    return true;
}

bool starts_with_folded(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size()
        && ascii_iequals(value.substr(0, prefix.size()), prefix);
}

bool starts_with_name_word(std::string_view name, std::string_view handle) {
    std::size_t start = 0;
    while (start < name.size()) {
        while (start < name.size() && is_space(name[start])) {
            ++start;
        }
        const std::size_t end = start;
        while (start < name.size() && !is_space(name[start])) {
            ++start;
        }
        if (start > end
            && starts_with_folded(name.substr(end, start - end), handle)) {
            return true;
        }
    }
    return false;
}

} // namespace cha
