#pragma once

#include "app/application.h"
#include "daemon/scgi.h"
#include "daemon/session_tag.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include <nlohmann/json.hpp>

namespace cha::daemon {

struct ParsedChatRequest {
    std::string model;  // Stable forum ID for CHA sessions.
    std::string model_name;  // Client-facing model string.
    std::string user_text;
    bool stream{};
    std::optional<SessionTag> tag;
    std::optional<std::string> title;
};

// Sent as `{"error": {"message", "type", "code"}}`. The type follows from
// the status.
struct ApiError {
    int status{};
    std::string message;
    std::string code;
};

[[nodiscard]] nlohmann::json openai_error(const ApiError& error);
bool write_error(
    int fd, const ApiError& error, const std::atomic<bool>& stop);

[[nodiscard]] std::variant<ParsedChatRequest, ApiError> parse_chat_request(
    std::string_view body, app::Application& application);

[[nodiscard]] nlohmann::json models_list(app::Application& application);

// Tests replace the clock that times SSE keepalives.
using TurnClock = std::function<std::chrono::steady_clock::time_point()>;

// Serves one request. The signal handler sets `stop`; this function also
// sets it when the daemon must not serve more requests.
void handle_request(
    app::Application& application,
    const ScgiRequest& request,
    int fd,
    std::atomic<bool>& stop,
    const TurnClock& clock = {});

} // namespace cha::daemon
