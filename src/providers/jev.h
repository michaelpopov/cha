#pragma once

#include "workspace/workspace.h"
#include "util/wake_notifier.h"
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <nlohmann/json.hpp>

namespace cha {
inline constexpr auto jev_request_timeout = std::chrono::seconds{5};

struct JevOption {
    std::string key;
    std::string character_id;
    std::string display_name;
};
struct JevRequestInput {
    WorkspaceJev config;
    std::string prompt;
    std::vector<JevOption> characters;
    std::chrono::steady_clock::time_point deadline{
        std::chrono::steady_clock::time_point::max()};
};
enum class JevOutcome { success, cancelled, failure };
enum class JevSearch { none, direct, rewrite };
struct JevResult {
    JevOutcome outcome{JevOutcome::failure};
    std::string choice;
    std::string message;
    std::optional<JevSearch> search_choice;
};
using JevExecutor = std::function<JevResult(const JevRequestInput&, const std::atomic_bool&)>;

nlohmann::ordered_json make_jev_body(const JevRequestInput& input);
JevResult parse_jev_result(const nlohmann::json& response, const JevRequestInput& input);
JevResult classify_jev(const WorkspaceJev& config, std::string key,
    const JevRequestInput& input, const std::atomic_bool& cancelled);

class JevRequest final {
public:
    void cancel() noexcept;
    std::optional<JevResult> try_receive();
private:
    friend class Providers;
    JevRequest(JevRequestInput input, std::shared_ptr<WakeNotifier> notifier);
    void publish(JevResult result);
    void execute(const JevExecutor& executor) noexcept;
    JevRequestInput input_;
    std::shared_ptr<WakeNotifier> notifier_;
    std::atomic_bool cancelled_{};
    std::mutex mutex_;
    bool published_{};
    std::optional<JevResult> result_;
};
} // namespace cha
