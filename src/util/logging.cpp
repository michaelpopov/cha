#include "util/logging.h"

#include "util/path_name.h"

#include <spdlog/sinks/base_sink.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cha {
namespace {

constexpr std::size_t log_file_size_limit = 10U * 1024U * 1024U;
constexpr std::size_t log_file_count = 3;
constexpr const char* log_pattern = "[%Y-%m-%d %H:%M:%S.%f] [thread %t] [%l] %v";

LogSeverity severity_from_spdlog(spdlog::level::level_enum level) {
    switch (level) {
    case spdlog::level::trace: return LogSeverity::trace;
    case spdlog::level::debug: return LogSeverity::debug;
    case spdlog::level::info: return LogSeverity::info;
    case spdlog::level::warn: return LogSeverity::warn;
    case spdlog::level::err: return LogSeverity::error;
    case spdlog::level::critical: return LogSeverity::critical;
    default: return LogSeverity::info;
    }
}

class MemoryLogSink : public spdlog::sinks::base_sink<std::mutex> {
public:
    MemoryLogSink() { set_level(spdlog::level::info); }

    LogBufferSnapshot snapshot() {
        std::lock_guard lock(mutex_);
        expire();
        LogBufferSnapshot result;
        result.entries.assign(entries_.begin(), entries_.end());
        result.latest_number = latest_number_;
        return result;
    }

    LogBufferState state() {
        std::lock_guard lock(mutex_);
        expire();
        return {
            .level = severity_from_spdlog(level()),
            .verbose_until = verbose_until_,
            .latest_number = latest_number_,
        };
    }

    void clear() {
        std::lock_guard lock(mutex_);
        entries_.clear();
    }

    void set_verbose(bool enabled) {
        std::lock_guard lock(mutex_);
        if (enabled) {
            verbose_until_ = clock_() + diagnostic_log_verbose_duration;
            set_level(spdlog::level::debug);
        } else {
            verbose_until_.reset();
            set_level(spdlog::level::info);
        }
    }

    void set_clock(std::function<std::chrono::steady_clock::time_point()> clock) {
        std::lock_guard lock(mutex_);
        if (clock) clock_ = std::move(clock);
        else {
            clock_ = [] { return std::chrono::steady_clock::now(); };
        }
    }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        expire();
        if (!should_log(msg.level)) return;
        spdlog::memory_buf_t formatted;
        formatter_->format(msg, formatted);
        std::string text(formatted.data(), formatted.size());
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        entries_.push_back(
            {++latest_number_, severity_from_spdlog(msg.level), std::move(text)});
        if (entries_.size() > diagnostic_log_capacity) entries_.pop_front();
    }

    void flush_() override {}

private:
    void expire() {
        if (!verbose_until_ || clock_() < *verbose_until_) return;
        verbose_until_.reset();
        set_level(spdlog::level::info);
    }

    std::deque<LogBufferEntry> entries_;
    std::uint64_t latest_number_{};
    std::optional<std::chrono::steady_clock::time_point> verbose_until_;
    std::function<std::chrono::steady_clock::time_point()> clock_{
        [] { return std::chrono::steady_clock::now(); }};
};

std::shared_ptr<spdlog::logger> diagnostic_logger;
std::shared_ptr<MemoryLogSink> memory_sink;
std::shared_ptr<spdlog::sinks::sink> payload_sink;
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

void write_log(spdlog::level::level_enum level, std::string_view message) noexcept {
    try {
        std::shared_ptr<spdlog::logger> logger;
        {
            std::lock_guard lock(diagnostic_logger_mutex);
            logger = diagnostic_logger;
        }
        if (logger) {
            logger->log(level, "{}", message);
        }
    } catch (...) {
        // A diagnostic sink must not affect application work.
    }
}

std::shared_ptr<MemoryLogSink> locked_memory_sink() {
    std::lock_guard lock(diagnostic_logger_mutex);
    return memory_sink;
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

    auto buffer = std::make_shared<MemoryLogSink>();
    std::vector<spdlog::sink_ptr> sinks{buffer};
    std::shared_ptr<spdlog::sinks::sink> file_sink;
    if (level != spdlog::level::off) {
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
        auto rotating = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            log_file.native(),
            log_file_size_limit,
            log_file_count,
            false);
        rotating->set_level(level);
        file_sink = rotating;
        sinks.push_back(rotating);
    }

    auto logger = std::make_shared<spdlog::logger>(
        "cha", sinks.begin(), sinks.end());
    logger->set_level(spdlog::level::trace);
    logger->set_pattern(log_pattern);
    logger->set_error_handler([](const std::string&) {
        // A diagnostic sink must not write to the server's standard streams.
    });
    logger->flush_on(spdlog::level::trace);
    spdlog::register_logger(logger);
    diagnostic_logger = logger;
    memory_sink = std::move(buffer);
    payload_sink = std::move(file_sink);
    if (level != spdlog::level::off) {
        logger->info("diagnostic logging enabled");
    }
}

void shutdown_diagnostic_logging() noexcept {
    try {
        std::lock_guard lock(diagnostic_logger_mutex);
        diagnostic_logger.reset();
        memory_sink.reset();
        payload_sink.reset();
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

bool debug_logging_enabled() noexcept {
    try {
        std::lock_guard lock(diagnostic_logger_mutex);
        return payload_sink && payload_sink->should_log(spdlog::level::debug);
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
        std::lock_guard lock(diagnostic_logger_mutex);
        if (!payload_sink) return;
        spdlog::details::log_msg message(
            spdlog::source_loc{}, "cha", spdlog::level::debug, line);
        payload_sink->log(message);
        payload_sink->flush();
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

LogBufferSnapshot snapshot_diagnostic_log() {
    const auto sink = locked_memory_sink();
    if (!sink) return {};
    return sink->snapshot();
}

LogBufferState diagnostic_log_state() {
    const auto sink = locked_memory_sink();
    if (!sink) return {};
    return sink->state();
}

void clear_diagnostic_log() {
    const auto sink = locked_memory_sink();
    if (sink) sink->clear();
}

void set_diagnostic_log_verbose(bool enabled) {
    const auto sink = locked_memory_sink();
    if (sink) sink->set_verbose(enabled);
}

void set_diagnostic_log_clock_for_test(
    std::function<std::chrono::steady_clock::time_point()> clock) {
    const auto sink = locked_memory_sink();
    if (sink) sink->set_clock(std::move(clock));
}

} // namespace cha
