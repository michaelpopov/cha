#include "util/logging.h"

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace cha {
namespace {

constexpr std::size_t log_file_size_limit = 10U * 1024U * 1024U;
constexpr std::size_t log_file_count = 3;
constexpr const char* log_pattern = "[%Y-%m-%d %H:%M:%S.%f] [thread %t] [%l] %v";

std::shared_ptr<spdlog::logger> diagnostic_logger;
std::filesystem::path diagnostic_file;
std::mutex diagnostic_logger_mutex;

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

void set_level_locked(spdlog::level::level_enum level) {
    if (level != spdlog::level::off && diagnostic_logger->sinks().empty()) {
        const auto directory = diagnostic_file.parent_path();
        if (!directory.empty()) std::filesystem::create_directories(directory);
        auto sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            diagnostic_file.native(), log_file_size_limit, log_file_count, false);
        sink->set_pattern(log_pattern);
        diagnostic_logger->sinks().push_back(std::move(sink));
    }
    diagnostic_logger->set_level(level);
}

void write_log(spdlog::level::level_enum level, std::string_view message) noexcept {
    try {
        std::lock_guard lock(diagnostic_logger_mutex);
        if (!diagnostic_logger || !diagnostic_logger->should_log(level)) return;
        // Keep each call on one physical line, including untrusted error text.
        std::string line;
        for (const char c : message) {
            if (c == '\n') line += "\\n";
            else if (c == '\r') line += "\\r";
            else line += c;
        }
        diagnostic_logger->log(level, "{}", line);
    } catch (...) {
        // A diagnostic sink must not affect application work.
    }
}

} // namespace

void initialize_diagnostic_logging(
    const std::filesystem::path& log_file,
    std::string_view log_level) {
    const spdlog::level::level_enum level = parse_log_level(log_level);

    std::lock_guard lock(diagnostic_logger_mutex);
    if (diagnostic_logger) {
        return;
    }

    diagnostic_file = log_file;
    diagnostic_logger = std::make_shared<spdlog::logger>(
        "cha", spdlog::sinks_init_list{});
    diagnostic_logger->set_error_handler([](const std::string&) {
        // A diagnostic sink must not write to the server's standard streams.
    });
    diagnostic_logger->flush_on(spdlog::level::trace);
    try {
        set_level_locked(level);
    } catch (...) {
        diagnostic_logger.reset();
        diagnostic_file.clear();
        throw;
    }
    diagnostic_logger->info("diagnostic logging enabled");
}

void shutdown_diagnostic_logging() noexcept {
    try {
        std::lock_guard lock(diagnostic_logger_mutex);
        diagnostic_logger.reset();
        diagnostic_file.clear();
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

bool debug_logging_enabled() noexcept {
    try {
        std::lock_guard lock(diagnostic_logger_mutex);
        return diagnostic_logger && diagnostic_logger->should_log(spdlog::level::debug);
    } catch (...) {
        return false;
    }
}

void log_debug_payload(std::string_view event, std::string_view payload,
    std::string_view credential) noexcept {
    if (payload.empty() || !debug_logging_enabled()) return;
    try {
        std::string text(payload);
        if (!credential.empty()) {
            std::size_t offset = 0;
            while ((offset = text.find(credential, offset)) != std::string::npos) {
                text.replace(offset, credential.size(), "[REDACTED]");
                offset += std::string_view("[REDACTED]").size();
            }
        }
        const std::string line = std::string(event) + " data=" + nlohmann::json(text).dump(
            -1, ' ', false, nlohmann::json::error_handler_t::replace);
        write_log(spdlog::level::debug, line);
    } catch (...) {
        // Payload diagnostics must not affect the request, even for invalid UTF-8.
    }
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

void set_diagnostic_log_level(std::string_view level) {
    const auto parsed = parse_log_level(level);
    std::lock_guard lock(diagnostic_logger_mutex);
    if (!diagnostic_logger) throw std::runtime_error("Diagnostic logging is not initialized.");
    set_level_locked(parsed);
}

std::string diagnostic_log_level() {
    std::lock_guard lock(diagnostic_logger_mutex);
    if (!diagnostic_logger) return "off";
    if (diagnostic_logger->level() == spdlog::level::warn) return "warn";
    const auto name = spdlog::level::to_string_view(diagnostic_logger->level());
    return {name.data(), name.size()};
}

std::filesystem::path diagnostic_log_file() {
    std::lock_guard lock(diagnostic_logger_mutex);
    return diagnostic_file;
}

} // namespace cha
