#include "web/session_markdown.h"

#include <algorithm>
#include <array>
#include <ctime>

namespace cha::web {
namespace {

std::string comment_text(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') continue;
        if (text[i] == '\n') {
            result.push_back(' ');
        } else {
            result.push_back(text[i]);
            if (text[i] == '-' && i + 1 < text.size() && text[i + 1] == '-') {
                result.push_back(' ');
            }
        }
    }
    return result;
}

std::string speaker_badge(std::string_view name) {
    std::size_t longest_run = 0;
    std::size_t run = 0;
    for (const char character : name) {
        if (character == '`') {
            longest_run = std::max(longest_run, ++run);
        } else {
            run = 0;
        }
    }

    const std::string delimiter(longest_run + 1, '`');
    const bool needs_padding = !name.empty()
        && (name.front() == '`' || name.back() == '`');
    return delimiter + (needs_padding ? " " : "") + std::string(name)
        + (needs_padding ? " " : "") + delimiter;
}

std::string compact_text(std::string_view source) {
    while (!source.empty() && (source.back() == '\n' || source.back() == '\r')) {
        source.remove_suffix(1);
    }

    std::string result;
    result.reserve(source.size());
    for (std::size_t i = 0; i < source.size();) {
        if (source[i] != '\n') {
            result.push_back(source[i++]);
            continue;
        }

        std::size_t end = i + 1;
        while (end < source.size() && source[end] == '\n') ++end;
        if (end - i > 1) result += "  ";
        result.push_back('\n');
        i = end;
    }
    return result;
}

std::string local_start_time(std::int64_t unix_seconds) {
    if (unix_seconds == 0) return {};

    const std::time_t time = static_cast<std::time_t>(unix_seconds);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &time) != 0) return {};
#else
    if (localtime_r(&time, &local) == nullptr) return {};
#endif

    std::array<char, 64> text{};
    if (std::strftime(text.data(), text.size(), "%B %d, %Y at %H:%M %Z", &local) == 0) {
        return {};
    }
    return text.data();
}

} // namespace

std::string session_markdown(
    std::string_view label,
    std::span<const TranscriptEntry> entries) {
    std::string result = "<!-- CHA session: " + comment_text(label) + " -->\n";
    for (const TranscriptEntry& entry : entries) {
        if (entry.kind == EntryKind::notice && entry.text.empty()) continue;
        if (const std::string timestamp = local_start_time(entry.created_at);
            !timestamp.empty()) {
            result += "*Started " + timestamp + "*\n";
            break;
        }
    }

    const TranscriptEntry* last_prompt = nullptr;
    for (const TranscriptEntry& entry : entries) {
        // Cover markers are transient, empty notice entries that
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
        result += "\n" + speaker_badge(entry.display_name) + " · ";
        result += compact_text(entry.text);
        result += '\n';
    }
    return result;
}

} // namespace cha::web
