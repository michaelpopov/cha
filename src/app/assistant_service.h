#pragma once

#include "providers/maintenance.h"
#include "runtime/protocol.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace cha {

class LiveSessionManager;
class OpenAiOAuth;
class SessionRepository;
class WorkspaceConfigStore;

// Welcome maintenance tools for vault rows, host TOML files, and memory logs.
// This type does not own the store or diagnostic sink.
class AssistantService {
public:
    struct Links {
        WorkspaceConfigStore* store{};
        OpenAiOAuth* oauth{};
        std::timed_mutex* lifecycle{};
        std::atomic_bool* stopping{};
        std::function<std::optional<ErrorCode>(std::uint64_t)> admit;
        LiveSessionManager* sessions{};
        SessionRepository* repository{};
        std::filesystem::path config_directory;
    };

    explicit AssistantService(Links links);

    void bind_runtime(LiveSessionManager& sessions, SessionRepository& repository);

    [[nodiscard]] std::string execute(
        std::string_view name,
        std::string_view arguments,
        const MaintenanceContext& context,
        const std::atomic_bool& cancelled);

private:
    Links links_;
};

} // namespace cha
