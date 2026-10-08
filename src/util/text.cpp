#include "util/text.h"

#include <algorithm>
#include <cctype>
#include <optional>

namespace cha {

std::string utf8_prefix(std::string_view value, std::size_t count) {
    count = std::min(count, value.size());
    while (count < value.size() && count > 0
        && (static_cast<unsigned char>(value[count]) & 0xc0) == 0x80) --count;
    return std::string(value.substr(0, count));
}

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

bool is_digit(char character) {
    return character >= '0' && character <= '9';
}

std::size_t skip_spaces(std::string_view text, std::size_t position) {
    while (position < text.size() && is_space(text[position])) ++position;
    return position;
}

// Only the text from `at` decides, never the text before it, so streamed
// chunks give the same result as the whole text.
bool starts_url(std::string_view text, std::size_t at) {
    const std::string_view rest = text.substr(at);
    return starts_with_folded(rest, "http://") || starts_with_folded(rest, "https://")
        || starts_with_folded(rest, "www.");
}

// Returns the end of a URL that starts at `at`, or `at` when none starts
// there. A URL can hold balanced parentheses, as Wikipedia URLs do.
std::size_t url_end(std::string_view text, std::size_t at, bool trim_punctuation) {
    if (!starts_url(text, at)) return at;
    std::size_t end = at;
    int depth = 0;
    for (; end < text.size(); ++end) {
        const char character = text[end];
        if (is_space(character)
            || std::string_view("<>[]\"`").find(character) != std::string_view::npos) {
            break;
        }
        if (character == '(') {
            ++depth;
        } else if (character == ')') {
            if (depth == 0) break;
            --depth;
        }
    }
    // Punctuation after a bare URL usually ends the sentence.
    while (trim_punctuation && end > at
        && std::string_view(".,;:!?'*_~").find(text[end - 1]) != std::string_view::npos) {
        --end;
    }
    return end;
}

struct MarkdownLink {
    std::size_t end;
    std::string_view label;
    std::string_view url;
    bool image;
};

// Parses "[label](url)" and "![label](url)", also with "<url>" or a title.
std::optional<MarkdownLink> markdown_link(std::string_view text, std::size_t at) {
    const bool image = at < text.size() && text[at] == '!';
    const std::size_t open = image ? at + 1 : at;
    if (open >= text.size() || text[open] != '[') return std::nullopt;
    const std::size_t close = text.find(']', open + 1);
    if (close == std::string_view::npos || close + 1 >= text.size() || text[close + 1] != '(') {
        return std::nullopt;
    }
    std::size_t position = skip_spaces(text, close + 2);
    const bool angle = position < text.size() && text[position] == '<';
    if (angle) ++position;
    const std::size_t url_start = position;
    position = url_end(text, url_start, false);
    if (position == url_start) return std::nullopt;
    const std::string_view url = text.substr(url_start, position - url_start);
    if (angle) {
        if (position >= text.size() || text[position] != '>') return std::nullopt;
        ++position;
    }
    position = skip_spaces(text, position);
    if (position < text.size() && text[position] == '"') {
        const std::size_t title_end = text.find('"', position + 1);
        if (title_end == std::string_view::npos) return std::nullopt;
        position = skip_spaces(text, title_end + 1);
    }
    if (position >= text.size() || text[position] != ')') return std::nullopt;
    return MarkdownLink{position + 1, text.substr(open + 1, close - open - 1), url, image};
}

// True when a link label only names its own address, as citations do:
// "[example.com](https://www.example.com/a)", "[1](...)", or a URL label.
bool names_the_link(std::string_view label, std::string_view url) {
    label = trim_view(label);
    if (label.empty() || std::ranges::all_of(label, is_digit) || starts_url(label, 0)) {
        return true;
    }
    const std::size_t scheme = url.find("://");
    std::string_view host = scheme == std::string_view::npos ? url : url.substr(scheme + 3);
    host = host.substr(0, host.find_first_of("/?#:"));
    const auto site_name = [](std::string_view value) {
        std::string folded = fold_ascii(value);
        if (folded.starts_with("www.")) folded.erase(0, 4);
        return folded;
    };
    const std::string site = site_name(host);
    const std::string name = site_name(label);
    return site == name || site.ends_with("." + name);
}

// Returns the end of "<url>", or `at` when there is none.
std::size_t angle_link_end(std::string_view text, std::size_t at) {
    if (at >= text.size() || text[at] != '<') return at;
    const std::size_t end = url_end(text, at + 1, false);
    return end > at + 1 && end < text.size() && text[end] == '>' ? end + 1 : at;
}

std::size_t link_end(std::string_view text, std::size_t at) {
    if (const auto link = markdown_link(text, at)) return link->end;
    if (const std::size_t end = angle_link_end(text, at); end > at) return end;
    return url_end(text, at, true);
}

// Returns the end of a parenthesis or bracket that holds only links, such as
// the citation "([example.com](https://example.com))", or `at` when there is none.
std::size_t link_group_end(std::string_view text, std::size_t at) {
    const char close = text[at] == '(' ? ')' : ']';
    std::size_t position = skip_spaces(text, at + 1);
    std::size_t end = link_end(text, position);
    if (end == position) return at;
    do {
        position = skip_spaces(text, end);
        if (position < text.size() && (text[position] == ',' || text[position] == ';')) {
            position = skip_spaces(text, position + 1);
        }
        end = link_end(text, position);
    } while (end > position);
    return position < text.size() && text[position] == close ? position + 1 : at;
}

struct UrlReference {
    std::size_t end;
    // The label of a descriptive link, which stays in the text.
    std::optional<std::string_view> label;
};

std::optional<UrlReference> url_reference_at(std::string_view text, std::size_t at) {
    if (text[at] == '(' || text[at] == '[') {
        if (const std::size_t end = link_group_end(text, at); end > at) return UrlReference{end};
    }
    if (const auto link = markdown_link(text, at)) {
        if (link->image || names_the_link(link->label, link->url)) return UrlReference{link->end};
        return UrlReference{link->end, link->label};
    }
    if (const std::size_t end = angle_link_end(text, at); end > at) return UrlReference{end};
    if (const std::size_t end = url_end(text, at, true); end > at) return UrlReference{end};
    return std::nullopt;
}

// Returns the end of the indentation and the list or quote marker that start a line.
std::size_t line_lead_end(std::string_view line) {
    std::size_t position = skip_spaces(line, 0);
    if (position < line.size()
        && std::string_view("-*+>").find(line[position]) != std::string_view::npos) {
        ++position;
    } else {
        std::size_t digits = position;
        while (digits < line.size() && digits - position < 9 && is_digit(line[digits])) ++digits;
        if (digits > position && digits < line.size()
            && (line[digits] == '.' || line[digits] == ')')) {
            position = digits + 1;
        }
    }
    return skip_spaces(line, position);
}

bool is_line_lead(std::string_view line) {
    return line_lead_end(line) == line.size();
}

// True for a Markdown reference definition such as "[1]: https://example.com".
bool is_reference_definition(std::string_view line) {
    const std::size_t open = skip_spaces(line, 0);
    if (open >= line.size() || line[open] != '[') return false;
    const std::size_t close = line.find(']', open + 1);
    if (close == std::string_view::npos || close == open + 1
        || close + 1 >= line.size() || line[close + 1] != ':') {
        return false;
    }
    std::size_t position = skip_spaces(line, close + 2);
    if (position < line.size() && line[position] == '<') ++position;
    return url_end(line, position, false) > position;
}

// Removes URL references from one line without its newline. `starts_line` is
// false when the line continues text that was already released.
std::string remove_line_urls(std::string_view line, bool starts_line, bool& removed) {
    std::string result;
    // The number of characters at the end of `result` copied unchanged
    // since the last removal.
    std::size_t copied = 0;
    std::size_t position = 0;
    while (position < line.size()) {
        const std::optional<UrlReference> reference = url_reference_at(line, position);
        if (!reference) {
            result.push_back(line[position++]);
            ++copied;
            continue;
        }
        removed = true;
        position = reference->end;
        if (reference->label) {
            result.append(*reference->label);
            copied = 0;
            continue;
        }
        // Drop emphasis, code, or quote marks that directly wrapped the removed text.
        while (copied > 0 && position < line.size() && result.back() == line[position]
            && std::string_view("*_`\"").find(line[position]) != std::string_view::npos) {
            result.pop_back();
            --copied;
            ++position;
        }
        copied = 0;
        if (starts_line && is_line_lead(result)) {
            // At the start of a line, drop the space after the removed text.
            position = skip_spaces(line, position);
        } else {
            while (!result.empty() && is_space(result.back())) result.pop_back();
        }
    }
    return result;
}

std::string remove_urls(std::string_view text, bool at_line_start) {
    std::string result;
    bool starts_line = at_line_start;
    std::size_t start = 0;
    while (true) {
        const std::size_t newline = text.find('\n', start);
        const std::string_view line = text.substr(
            start, newline == std::string_view::npos ? std::string_view::npos : newline - start);
        bool removed = false;
        std::string cleaned;
        if (starts_line && is_reference_definition(line)) {
            removed = true;
        } else {
            cleaned = remove_line_urls(line, starts_line, removed);
        }
        // A line that held only links disappears together with its newline.
        const bool keep = !(removed && starts_line && is_line_lead(cleaned));
        if (keep) result += cleaned;
        if (newline == std::string_view::npos) return result;
        if (keep) result += '\n';
        start = newline + 1;
        starts_line = true;
    }
}

// True when more text can turn the end of a line into the start of a link:
// a partial "http" or "www.", an image "!", or a mark that can wrap a URL.
bool may_start_link(std::string_view line) {
    if (line.empty()) return false;
    if (std::string_view("!*_`\"").find(line.back()) != std::string_view::npos) return true;
    for (const std::string_view start : {"http", "www."}) {
        for (std::size_t size = 1; size < start.size() && size <= line.size(); ++size) {
            if (ascii_iequals(line.substr(line.size() - size), start.substr(0, size))) return true;
        }
    }
    return false;
}

// Returns where the part of a line starts that can still become a link:
// trailing spaces, which a removal can take, a last word that can grow into
// a link, and everything from the word where a link can start.
std::size_t held_line_start(std::string_view line) {
    const auto word_start = [line](std::size_t position) {
        while (position > 0 && !is_space(line[position - 1])) --position;
        while (position > 0 && is_space(line[position - 1])) --position;
        return position;
    };
    std::size_t hold = line.size();
    while (hold > 0 && is_space(line[hold - 1])) --hold;
    if (may_start_link(line)) hold = word_start(line.size());
    const std::string folded = fold_ascii(line);
    const std::size_t link = std::min(
        {line.find_first_of("([<"), folded.find("http"), folded.find("www.")});
    if (link != std::string_view::npos) hold = std::min(hold, word_start(link));
    return hold;
}

std::size_t releasable_prefix(std::string_view text, bool at_line_start) {
    const std::size_t newline = text.rfind('\n');
    const std::size_t line_start = newline == std::string_view::npos ? 0 : newline + 1;
    const std::string_view line = text.substr(line_start);
    std::size_t hold = held_line_start(line);
    // Hold a list marker with its line, so a line of links can disappear
    // whole. Digits alone can still become a marker such as "1.".
    if ((line_start > 0 || at_line_start)
        && (hold <= line_lead_end(line)
            || line.substr(0, hold).find_first_not_of(" \t0123456789")
                == std::string_view::npos)) {
        hold = 0;
    }
    return line_start + hold;
}

} // namespace

std::string remove_url_references(std::string_view value) {
    return remove_urls(value, true);
}

std::string UrlReferenceFilter::push(std::string_view text) {
    pending_.append(text);
    const std::size_t size = releasable_prefix(pending_, at_line_start_);
    if (size == 0) return {};
    std::string result = remove_urls(std::string_view(pending_).substr(0, size), at_line_start_);
    at_line_start_ = pending_[size - 1] == '\n';
    pending_.erase(0, size);
    return result;
}

std::string UrlReferenceFilter::finish() {
    std::string result = remove_urls(pending_, at_line_start_);
    pending_.clear();
    at_line_start_ = true;
    return result;
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
