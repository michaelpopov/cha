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
    std::string model;
    std::string user_text;
    bool stream{};
    std::optional<SessionTag> tag;
};

struct ChatParseError {
    int status{};
    std::string message;
    std::string type;
    std::string code;
};

[[nodiscard]] nlohmann::json openai_error(
    std::string_view message,
    std::string_view type,
    std::string_view code);

[[nodiscard]] std::variant<ParsedChatRequest, ChatParseError>
parse_chat_request(
    std::string_view body, app::Application& application);

[[nodiscard]] nlohmann::json models_list(app::Application& application);

// The signal handler only stores `stop`. Budget timing stays on the
// request thread so one shutdown grace covers drain and join.
struct DaemonShutdown {
    explicit DaemonShutdown(
        std::atomic<bool>& stop_flag,
        std::chrono::milliseconds shutdown_grace =
            std::chrono::milliseconds{10000});

    void arm_budget();
    void request_stop();
    void disarm_if_idle();
    [[nodiscard]] bool requested() const;
    [[nodiscard]] bool expired() const;
    [[nodiscard]] std::chrono::milliseconds remaining() const;

    std::atomic<bool>& stop;
    std::chrono::milliseconds grace;
    std::chrono::steady_clock::time_point deadline{
        std::chrono::steady_clock::time_point::max()};
    bool armed{false};
};

using TurnClock = std::function<std::chrono::steady_clock::time_point()>;

void handle_request(
    app::Application& application,
    const ScgiRequest& request,
    int fd,
    DaemonShutdown& shutdown,
    const TurnClock& clock = {});

} // namespace cha::daemon
