#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace cha {

inline constexpr int maintenance_call_limit = 24;
inline constexpr std::size_t maintenance_argument_limit = 256 * 1024;
inline constexpr std::size_t maintenance_call_result_limit = 256 * 1024;
inline constexpr std::size_t maintenance_answer_result_limit = 512 * 1024;
inline constexpr std::size_t ordinary_tool_argument_limit = 16 * 1024;
inline constexpr std::string_view maintenance_output_limit_message =
    "The model's output limit cut off the tool call. This call was not applied. "
    "Edit the configuration file manually.";

// Native identity for one Welcome request. The model cannot supply these values.
struct MaintenanceContext {
    std::string character_id;
    std::string forum_id;
    std::string session_id;
    std::uint64_t request_id{};
    std::uint64_t context_epoch{};
};

// Returns one JSON object. Workers own a copy and pass the captured context.
using MaintenanceExecutor = std::function<std::string(
    std::string_view name,
    std::string_view arguments,
    const MaintenanceContext& context,
    const std::atomic_bool& cancelled)>;

// Host line, then the three embedded maintenance documents.
std::string maintenance_reference(bool chaweb_host);

} // namespace cha
