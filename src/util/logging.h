#pragma once

#include <filesystem>
#include <string_view>

namespace cha {

// Enables the named, file-only diagnostic logger using the application
// configuration. Missing parent directories are created before the file opens.
// Call this once before worker threads are started.
void initialize_diagnostic_logging(
    const std::filesystem::path& log_file,
    std::string_view log_level);

// Releases the process logger. Applications normally rely on process exit;
// this exists for orderly test isolation.
void shutdown_diagnostic_logging() noexcept;

// These calls are best-effort: logging failures never affect application work.
void log_trace(std::string_view message) noexcept;
void log_debug(std::string_view message) noexcept;
void log_info(std::string_view message) noexcept;
void log_warn(std::string_view message) noexcept;
void log_error(std::string_view message) noexcept;
void log_critical(std::string_view message) noexcept;

} // namespace cha
