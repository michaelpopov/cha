#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace cha {

bool is_space(char character);
std::size_t find_whitespace(std::string_view value);
std::string_view trim_view(std::string_view value);
std::string fold_ascii(std::string_view value);
// Removes links and URLs from model text. A Markdown link with a descriptive
// label keeps its label. Citations, images, bare URLs, and reference
// definitions disappear together with the space before them, and a line
// that holds nothing else disappears completely.
std::string remove_url_references(std::string_view value);

// Removes URL references from streamed model text. It holds back only text
// that can still become part of a link: trailing spaces, a last word that can
// grow into a link, and the rest of a line from a possible link start until
// the line ends.
class UrlReferenceFilter {
public:
    std::string push(std::string_view text);
    std::string finish();

private:
    std::string pending_;
    bool at_line_start_{true};
};

// Case-insensitive handle matching, shared by the character and persona
// resolvers so both spell "same name" the same way. These compare in place
// rather than folding into a temporary.
bool ascii_iequals(std::string_view left, std::string_view right);
bool starts_with_folded(std::string_view value, std::string_view prefix);
// True when any whitespace-separated word of `name` starts with `handle`.
bool starts_with_name_word(std::string_view name, std::string_view handle);

} // namespace cha
