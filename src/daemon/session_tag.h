#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json_fwd.hpp>

namespace cha::daemon {

struct SessionTag {
    std::string forum_id;
    std::string session_id;
    bool operator==(const SessionTag&) const = default;
};

enum class SessionTagScan {
    no_assistant,
    found,
    missing,
};

struct SessionTagScanResult {
    SessionTagScan status{SessionTagScan::no_assistant};
    SessionTag tag;
};

[[nodiscard]] std::string format_session_tag(
    std::string_view forum_id, std::string_view session_id);
[[nodiscard]] std::string message_text(const nlohmann::json& message);
[[nodiscard]] std::optional<SessionTag> parse_session_tag_text(
    std::string_view text);
[[nodiscard]] SessionTagScanResult find_session_tag(
    const nlohmann::json& messages);

} // namespace cha::daemon
