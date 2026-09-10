#include "util/text.h"

#include <cctype>
#include <regex>

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

constexpr std::string_view source_reference_start = "([";
constexpr std::size_t maximum_source_reference_size = 512;

const std::regex& source_reference_pattern() {
    static const std::regex pattern(
        R"(\(\[[^\]\r\n]+\]\(https?://[^)\r\n]+\)\))",
        std::regex::ECMAScript | std::regex::icase);
    return pattern;
}

std::size_t possible_source_reference_start(std::string_view text) {
    if (!text.empty() && text.back() == '(') return text.size() - 1;
    return text.size();
}

char fold_character(char value) {
    return value >= 'A' && value <= 'Z'
        ? static_cast<char>(value - 'A' + 'a')
        : value;
}

} // namespace

std::size_t complete_source_reference_prefix(std::string_view value) {
    std::size_t position = 0;
    while (true) {
        const std::size_t start = value.find(source_reference_start, position);
        if (start == std::string_view::npos) {
            const std::string_view tail = value.substr(position);
            return position + possible_source_reference_start(tail);
        }

        const std::string_view candidate = value.substr(start);
        std::match_results<std::string_view::const_iterator> match;
        if (std::regex_search(
                candidate.begin(), candidate.end(), match,
                source_reference_pattern(),
                std::regex_constants::match_continuous)) {
            position = start + match.length();
            continue;
        }

        const std::size_t next =
            candidate.find(source_reference_start, source_reference_start.size());
        if (next != std::string_view::npos) {
            position = start + next;
            continue;
        }
        // A normal parenthetical is already known not to be a source
        // reference and need not stall streaming while more prose arrives.
        const std::size_t literal_end =
            candidate.find("])", source_reference_start.size());
        if (literal_end != std::string_view::npos) {
            position = start + literal_end + 2;
            continue;
        }
        if (candidate.size() > maximum_source_reference_size) {
            position = start + source_reference_start.size();
            continue;
        }
        return start;
    }
}

std::string remove_source_references(std::string_view value) {
    return std::regex_replace(
        std::string(value), source_reference_pattern(), std::string{});
}

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
