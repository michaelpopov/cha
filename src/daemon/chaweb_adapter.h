#pragma once

#include "app/application.h"
#include "daemon/scgi.h"

#include <atomic>
#include <optional>
#include <string_view>

namespace cha::daemon {

// Tests replace session deletion after a failed create.
using ChaWebDeleteSession = std::optional<ErrorCode> (*)(
    app::Application& application,
    std::string_view forum_id,
    std::string_view session_id,
    std::uint64_t epoch);

void handle_chaweb_request(
    app::Application& application,
    const ScgiRequest& request,
    int fd,
    std::atomic<bool>& stop,
    ChaWebDeleteSession delete_session = nullptr);

} // namespace cha::daemon
