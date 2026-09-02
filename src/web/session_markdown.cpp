#include "web/session_markdown.h"

#include <array>
#include <ctime>

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

std::string local_timestamp(std::int64_t unix_seconds) {
    if (unix_seconds == 0) return {};

    const std::time_t time = static_cast<std::time_t>(unix_seconds);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &time) != 0) return {};
#else
    if (localtime_r(&time, &local) == nullptr) return {};
#endif

    std::array<char, 64> text{};
    if (std::strftime(text.data(), text.size(), "%B %d, %Y, %H:%M %Z", &local) == 0) {
        return {};
    }
    return text.data();
}

} // namespace

std::string session_markdown(
    std::string_view label,
    std::span<const TranscriptEntry> entries) {
    std::string result = "# " + heading_text(label) + "\n";
    const TranscriptEntry* last_prompt = nullptr;
    for (const TranscriptEntry& entry : entries) {
        // Off-record markers (/hide) are transient, empty notice entries that
        // exist only to draw a boundary in the live view; they carry no content
        // to export and would otherwise appear as blank speaker headings.
        if (entry.kind == EntryKind::notice && entry.text.empty()) continue;
        if (entry.kind == EntryKind::human) {
            const bool repeated = last_prompt != nullptr
                && last_prompt->participant_id == entry.participant_id
                && last_prompt->text == entry.text;
            last_prompt = &entry;
            if (repeated) continue;
        }
        result += "\n## " + heading_text(entry.display_name) + "\n";
        if (const std::string timestamp = local_timestamp(entry.created_at);
            !timestamp.empty()) {
            result += "*" + timestamp + "*\n\n";
        } else {
            result += '\n';
        }
        result += entry.text;
        result += '\n';
    }
    return result;
}

} // namespace cha::web
