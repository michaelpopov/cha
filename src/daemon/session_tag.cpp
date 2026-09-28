#include "daemon/session_tag.h"

#include "util/path_name.h"

#include <cctype>
#include <string>

#include <nlohmann/json.hpp>

namespace cha::daemon {
namespace {

constexpr std::string_view tag_prefix = "[//]: # (cha ";

std::string_view first_line_after_whitespace(std::string_view text) {
    const std::size_t end = text.find('\n');
    std::string_view line =
        end == std::string_view::npos ? text : text.substr(0, end);
    if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
    }
    std::size_t index = 0;
    while (index < line.size()) {
        const unsigned char character =
            static_cast<unsigned char>(line[index]);
        if (std::isspace(character) == 0) break;
        ++index;
    }
    return line.substr(index);
}

} // namespace

std::string format_session_tag(
    std::string_view forum_id, std::string_view session_id) {
    std::string line;
    line.reserve(tag_prefix.size() + forum_id.size() + session_id.size() + 2);
    line.append(tag_prefix);
    line.append(forum_id);
    line.push_back('/');
    line.append(session_id);
    line.push_back(')');
    return line;
}

std::string message_text(const nlohmann::json& message) {
    if (!message.is_object() || !message.contains("content")) return {};
    const nlohmann::json& content = message["content"];
    if (content.is_string()) return content.get<std::string>();
    if (!content.is_array()) return {};
    std::string text;
    for (const nlohmann::json& part : content) {
        if (!part.is_object()) continue;
        if (!part.contains("type") || !part["type"].is_string()
            || part["type"] != "text") {
            continue;
        }
        if (!part.contains("text") || !part["text"].is_string()) continue;
        text += part["text"].get<std::string>();
    }
    return text;
}

std::optional<SessionTag> parse_session_tag_text(std::string_view text) {
    const std::string_view line = first_line_after_whitespace(text);
    if (!line.starts_with(tag_prefix) || line.size() < tag_prefix.size() + 1
        || line.back() != ')') {
        return std::nullopt;
    }
    const std::string_view inner = line.substr(
        tag_prefix.size(), line.size() - tag_prefix.size() - 1);
    const std::size_t slash = inner.find('/');
    if (slash == std::string_view::npos
        || inner.find('/', slash + 1) != std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view forum_id = inner.substr(0, slash);
    const std::string_view session_id = inner.substr(slash + 1);
    if (!is_url_safe_identifier(forum_id)
        || !is_url_safe_identifier(session_id)) {
        return std::nullopt;
    }
    return SessionTag{
        .forum_id = std::string(forum_id),
        .session_id = std::string(session_id),
    };
}

SessionTagScanResult find_session_tag(const nlohmann::json& messages) {
    SessionTagScanResult result;
    if (!messages.is_array()) {
        result.status = SessionTagScan::missing;
        return result;
    }
    bool saw_assistant = false;
    for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
        if (!it->is_object()) continue;
        if (!it->contains("role") || (*it)["role"] != "assistant") continue;
        saw_assistant = true;
        if (auto tag = parse_session_tag_text(message_text(*it))) {
            result.status = SessionTagScan::found;
            result.tag = std::move(*tag);
            return result;
        }
    }
    result.status = saw_assistant
        ? SessionTagScan::missing
        : SessionTagScan::no_assistant;
    return result;
}

} // namespace cha::daemon
