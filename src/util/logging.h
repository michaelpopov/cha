#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

enum class LogSeverity {
    trace,
    debug,
    info,
    warn,
    error,
    critical,
};

struct LogBufferEntry {
    std::uint64_t number{};
    LogSeverity level{LogSeverity::info};
    std::string text;
};

struct LogBufferSnapshot {
    std::vector<LogBufferEntry> entries;
    std::uint64_t latest_number{};
};

struct LogBufferState {
    LogSeverity level{LogSeverity::info};
    std::optional<std::chrono::steady_clock::time_point> verbose_until;
    std::uint64_t latest_number{};
    // Milliseconds left on the sink clock. Null when verbose logging is off.
    std::optional<std::int64_t> verbose_remaining_ms;
};

inline constexpr std::size_t diagnostic_log_capacity = 2000;
inline constexpr auto diagnostic_log_verbose_duration = std::chrono::minutes(5);

// Enables the process diagnostic logger. The memory buffer is attached even
// when file logging is off. Missing parent directories are created before the
// file opens. Call this once before worker threads are started.
void initialize_diagnostic_logging(
    const std::filesystem::path& log_file,
    std::string_view log_level);

// Releases the process logger. Applications normally rely on process exit;
// this exists for orderly test isolation.
void shutdown_diagnostic_logging() noexcept;

// These calls are best-effort: logging failures never affect application work.
void log_trace(std::string_view message) noexcept;
void log_debug(std::string_view message) noexcept;
bool debug_logging_enabled() noexcept;
// Quotes payloads onto one log line and redacts the active request credential.
void log_debug_payload(std::string_view event, std::string_view payload,
    std::string_view credential = {}) noexcept;
void log_info(std::string_view message) noexcept;
void log_warn(std::string_view message) noexcept;
void log_error(std::string_view message) noexcept;
void log_critical(std::string_view message) noexcept;

LogBufferSnapshot snapshot_diagnostic_log();
LogBufferState diagnostic_log_state();
void clear_diagnostic_log();
void set_diagnostic_log_verbose(bool enabled);
void set_diagnostic_log_clock_for_test(
    std::function<std::chrono::steady_clock::time_point()> clock);

} // namespace cha
