#include "util/logging.h"

#include "util/path_name.h"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>

namespace cha {
namespace {

constexpr std::size_t log_file_size_limit = 10U * 1024U * 1024U;
constexpr std::size_t log_file_count = 3;

std::atomic<std::shared_ptr<spdlog::logger>> diagnostic_logger;
std::mutex diagnostic_logger_initialization;

spdlog::level::level_enum parse_log_level(std::string_view value) {
    if (value == "trace") {
        return spdlog::level::trace;
    }
    if (value == "debug") {
        return spdlog::level::debug;
    }
    if (value == "info") {
        return spdlog::level::info;
    }
    if (value == "warn") {
        return spdlog::level::warn;
    }
    if (value == "error") {
        return spdlog::level::err;
    }
    if (value == "critical") {
        return spdlog::level::critical;
    }
    if (value == "off") {
        return spdlog::level::off;
    }
    throw std::runtime_error(
        "Unsupported logging level '" + std::string(value)
        + "'; expected trace, debug, info, warn, error, critical, or off");
}

void write_log(spdlog::level::level_enum level, std::string_view message) noexcept {
    try {
        if (const auto logger = diagnostic_logger.load(std::memory_order_acquire)) {
            logger->log(level, "{}", message);
        }
    } catch (...) {
        // A diagnostic sink must not affect application work.
    }
}

} // namespace

void initialize_diagnostic_logging(
    const std::filesystem::path& log_file,
    std::string_view log_level) {
    const spdlog::level::level_enum level = parse_log_level(log_level);
    if (level == spdlog::level::off) {
        return;
    }

    std::lock_guard lock(diagnostic_logger_initialization);
    if (diagnostic_logger.load(std::memory_order_relaxed)) {
        return;
    }

    const std::filesystem::path directory = log_file.parent_path();
    if (!directory.empty()) {
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) {
            throw std::runtime_error(
                "Failed to create log directory '" + utf8_path(directory)
                + "': " + error.message());
        }
    }

    auto logger = spdlog::rotating_logger_mt(
        "cha",
        log_file.native(),
        log_file_size_limit,
        log_file_count,
        false);
    logger->set_level(level);
    logger->set_pattern(
        "[%Y-%m-%d %H:%M:%S.%f] [thread %t] [%l] %v");
    logger->set_error_handler([](const std::string&) {
        // A diagnostic sink must not write to the server's standard streams.
    });
    logger->flush_on(spdlog::level::trace);
    diagnostic_logger.store(logger, std::memory_order_release);
    logger->info("diagnostic logging enabled");
}

void shutdown_diagnostic_logging() noexcept {
    try {
        std::lock_guard lock(diagnostic_logger_initialization);
        diagnostic_logger.store({}, std::memory_order_release);
        spdlog::drop("cha");
    } catch (...) {
        // Teardown must not affect application exit or test cleanup.
    }
}

void log_trace(std::string_view message) noexcept {
    write_log(spdlog::level::trace, message);
}

void log_debug(std::string_view message) noexcept {
    write_log(spdlog::level::debug, message);
}

void log_info(std::string_view message) noexcept {
    write_log(spdlog::level::info, message);
}

void log_warn(std::string_view message) noexcept {
    write_log(spdlog::level::warn, message);
}

void log_error(std::string_view message) noexcept {
    write_log(spdlog::level::err, message);
}

void log_critical(std::string_view message) noexcept {
    write_log(spdlog::level::critical, message);
}

} // namespace cha
