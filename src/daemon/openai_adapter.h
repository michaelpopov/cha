#pragma once

#include "app/application.h"
#include "daemon/scgi.h"
#include "daemon/session_tag.h"

#include <atomic>
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

void handle_request(
    app::Application& application,
    const ScgiRequest& request,
    int fd,
    std::atomic<bool>& stop);

} // namespace cha::daemon
