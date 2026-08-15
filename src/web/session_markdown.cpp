#include "web/session_markdown.h"

namespace cha::web {
namespace {

std::string heading_text(std::string_view text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char character : text) {
        if (character == '\\' || character == '#' || character == '*'
            || character == '_' || character == '[' || character == ']'
            || character == '<' || character == '>') {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }
    return escaped;
}

} // namespace

std::string session_markdown(
    std::string_view label,
    std::span<const TranscriptEntry> entries) {
    std::string result = "# " + heading_text(label) + "\n";
    for (const TranscriptEntry& entry : entries) {
        // Off-record markers (/hide) are transient, empty notice entries that
        // exist only to draw a boundary in the live view; they carry no content
        // to export and would otherwise appear as blank speaker headings.
        if (entry.kind == EntryKind::notice && entry.text.empty()) continue;
        result += "\n## " + heading_text(entry.display_name) + "\n\n";
        result += entry.text;
        result += '\n';
    }
    return result;
}

} // namespace cha::web
