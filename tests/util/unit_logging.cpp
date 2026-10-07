#include "util/logging.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

namespace cha {
namespace {

class LoggingTest : public testing::Test {
protected:
    void SetUp() override {
        directory_ = std::filesystem::temp_directory_path()
            / ("cha_logging_"
               + std::to_string(++serial_)
               + "_"
               + std::to_string(reinterpret_cast<std::uintptr_t>(this))
               + "_"
               + std::to_string(
                   std::chrono::steady_clock::now().time_since_epoch().count()));
    }

    void TearDown() override {
        shutdown_diagnostic_logging();
        std::filesystem::remove_all(directory_);
    }

    std::filesystem::path log_file(std::string_view name = "cha.log") const {
        return directory_ / "diagnostics" / name;
    }

    static std::string contents(const std::filesystem::path& path) {
        std::ifstream input(path);
        return {
            std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
    }

private:
    std::filesystem::path directory_;
    static inline std::atomic_uint64_t serial_{};
};

TEST_F(LoggingTest, CreatesTheConfiguredParentDirectoryAndFile) {
    const std::filesystem::path path = log_file();

    initialize_diagnostic_logging(path, "info");
    log_info("informational event");
    log_warn("warning event");
    log_error("error event");
    log_critical("critical event");

    EXPECT_TRUE(std::filesystem::is_directory(path.parent_path()));
    EXPECT_TRUE(std::filesystem::is_regular_file(path));
    const std::string output = contents(path);
    EXPECT_NE(output.find("diagnostic logging enabled"), std::string::npos);
    EXPECT_NE(output.find("informational event"), std::string::npos);
    EXPECT_NE(output.find("warning event"), std::string::npos);
    EXPECT_NE(output.find("error event"), std::string::npos);
    EXPECT_NE(output.find("critical event"), std::string::npos);
}

TEST_F(LoggingTest, OffDoesNotCreateALogFile) {
    const std::filesystem::path path = log_file();

    initialize_diagnostic_logging(path, "off");
    log_error("not written");

    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST_F(LoggingTest, FiltersBelowTheConfiguredLevel) {
    const std::filesystem::path path = log_file();

    initialize_diagnostic_logging(path, "error");
    log_info("not written");
    log_error("written");

    const std::string output = contents(path);
    EXPECT_EQ(output.find("not written"), std::string::npos);
    EXPECT_NE(output.find("written"), std::string::npos);
}

TEST_F(LoggingTest, AllowsVerboseLevels) {
    const std::filesystem::path path = log_file();

    initialize_diagnostic_logging(path, "debug");
    log_debug("debug event");

    EXPECT_NE(contents(path).find("debug event"), std::string::npos);
}

TEST_F(LoggingTest, RejectsUnsupportedLevel) {
    EXPECT_THROW(
        initialize_diagnostic_logging(log_file(), "verbose"),
        std::runtime_error);
}

TEST_F(LoggingTest, DebugPayloadsAreEscapedAndCredentialsAreRedacted) {
    initialize_diagnostic_logging(log_file(), "debug");
    EXPECT_TRUE(debug_logging_enabled());
    log_debug_payload("empty payload", "");
    log_debug_payload("response", "first\n[error] forged credential-123 credential-123", "credential-123");
    const auto output = contents(log_file());
    EXPECT_EQ(output.find("empty payload"), std::string::npos);
    EXPECT_NE(output.find("first\\n[error] forged [REDACTED] [REDACTED]"), std::string::npos);
    EXPECT_EQ(output.find("\n[error] forged"), std::string::npos);
    EXPECT_EQ(output.find("credential-123"), std::string::npos);
}

TEST_F(LoggingTest, InfoLoggingDoesNotWriteDebugPayloads) {
    initialize_diagnostic_logging(log_file(), "info");
    EXPECT_FALSE(debug_logging_enabled());
    log_debug_payload("request", "private prompt");
    EXPECT_EQ(contents(log_file()).find("private prompt"), std::string::npos);
}

TEST_F(LoggingTest, IgnoresRepeatedInitialization) {
    const std::filesystem::path first = log_file("first.log");
    const std::filesystem::path second = log_file("second.log");

    initialize_diagnostic_logging(first, "info");
    initialize_diagnostic_logging(second, "info");
    log_info("written once");

    EXPECT_NE(contents(first).find("written once"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(second));
}

TEST_F(LoggingTest, BufferExistsWhenFileLoggingIsOff) {
    initialize_diagnostic_logging(log_file(), "off");
    log_info("buffered without a file");
    EXPECT_FALSE(std::filesystem::exists(log_file()));
    const auto snapshot = snapshot_diagnostic_log();
    ASSERT_EQ(snapshot.entries.size(), 1U);
    EXPECT_EQ(snapshot.entries.front().level, LogSeverity::info);
    EXPECT_NE(snapshot.entries.front().text.find("buffered without a file"), std::string::npos);
    EXPECT_EQ(snapshot.latest_number, snapshot.entries.front().number);
}

TEST_F(LoggingTest, FileAndBufferLevelsAreIndependent) {
    initialize_diagnostic_logging(log_file(), "error");
    log_info("info stays in the buffer");
    log_error("error reaches both");
    const auto output = contents(log_file());
    EXPECT_EQ(output.find("info stays in the buffer"), std::string::npos);
    EXPECT_NE(output.find("error reaches both"), std::string::npos);
    bool saw_info = false;
    bool saw_error = false;
    for (const auto& entry : snapshot_diagnostic_log().entries) {
        if (entry.text.find("info stays in the buffer") != std::string::npos) {
            saw_info = true;
        }
        if (entry.text.find("error reaches both") != std::string::npos) {
            saw_error = true;
        }
    }
    EXPECT_TRUE(saw_info);
    EXPECT_TRUE(saw_error);
}

TEST_F(LoggingTest, DebugPayloadsBypassTheMemoryBuffer) {
    initialize_diagnostic_logging(log_file(), "debug");
    EXPECT_TRUE(debug_logging_enabled());
    set_diagnostic_log_verbose(true);
    log_debug("ordinary debug");
    log_debug_payload("response", "private prompt");
    EXPECT_NE(contents(log_file()).find("private prompt"), std::string::npos);
    const auto snapshot = snapshot_diagnostic_log();
    bool saw_ordinary = false;
    for (const auto& entry : snapshot.entries) {
        EXPECT_EQ(entry.text.find("private prompt"), std::string::npos);
        if (entry.text.find("ordinary debug") != std::string::npos) saw_ordinary = true;
    }
    EXPECT_TRUE(saw_ordinary);
}

TEST_F(LoggingTest, VerboseBufferDoesNotEnableFilePayloads) {
    initialize_diagnostic_logging(log_file(), "info");
    EXPECT_FALSE(debug_logging_enabled());
    set_diagnostic_log_verbose(true);
    EXPECT_FALSE(debug_logging_enabled());
    log_debug("buffer debug");
    log_debug_payload("request", "private prompt");
    EXPECT_EQ(contents(log_file()).find("private prompt"), std::string::npos);
    EXPECT_EQ(contents(log_file()).find("buffer debug"), std::string::npos);
    bool saw_debug = false;
    for (const auto& entry : snapshot_diagnostic_log().entries) {
        if (entry.text.find("buffer debug") != std::string::npos) saw_debug = true;
        EXPECT_EQ(entry.text.find("private prompt"), std::string::npos);
    }
    EXPECT_TRUE(saw_debug);
}

TEST_F(LoggingTest, SnapshotClearAndNumbering) {
    initialize_diagnostic_logging(log_file(), "off");
    log_warn("first");
    log_error("second");
    const auto first = snapshot_diagnostic_log();
    ASSERT_EQ(first.entries.size(), 2U);
    EXPECT_EQ(first.entries[0].number + 1, first.entries[1].number);
    EXPECT_EQ(first.latest_number, first.entries[1].number);
    clear_diagnostic_log();
    const auto cleared = snapshot_diagnostic_log();
    EXPECT_TRUE(cleared.entries.empty());
    EXPECT_EQ(cleared.latest_number, first.latest_number);
    log_info("third");
    const auto after = snapshot_diagnostic_log();
    ASSERT_EQ(after.entries.size(), 1U);
    EXPECT_EQ(after.entries.front().number, first.latest_number + 1);
}

TEST_F(LoggingTest, RejectedMessagesDoNotAdvanceNumbersAndEvictOldest) {
    initialize_diagnostic_logging(log_file(), "off");
    log_debug("rejected");
    EXPECT_TRUE(snapshot_diagnostic_log().entries.empty());
    EXPECT_EQ(snapshot_diagnostic_log().latest_number, 0U);
    for (std::size_t index = 0; index < diagnostic_log_capacity + 5; ++index) {
        log_info("entry-" + std::to_string(index));
    }
    const auto snapshot = snapshot_diagnostic_log();
    ASSERT_EQ(snapshot.entries.size(), diagnostic_log_capacity);
    EXPECT_NE(snapshot.entries.front().text.find("entry-5"), std::string::npos);
    EXPECT_EQ(snapshot.latest_number, diagnostic_log_capacity + 5);
    EXPECT_EQ(snapshot.entries.back().number, snapshot.latest_number);
}

TEST_F(LoggingTest, CarriageReturnStaysOneEntry) {
    initialize_diagnostic_logging(log_file(), "off");
    log_warn("unknown field 'a\r\n[error] forged'");
    const auto snapshot = snapshot_diagnostic_log();
    ASSERT_EQ(snapshot.entries.size(), 1U);
    EXPECT_EQ(snapshot.entries.front().level, LogSeverity::warn);
    EXPECT_NE(snapshot.entries.front().text.find("a\r\n[error] forged"), std::string::npos);
}

TEST_F(LoggingTest, VerboseExpiryUsesTheInjectedClock) {
    initialize_diagnostic_logging(log_file(), "off");
    auto now = std::chrono::steady_clock::now();
    set_diagnostic_log_clock_for_test([&] { return now; });
    set_diagnostic_log_verbose(true);
    log_debug("accepted");
    EXPECT_EQ(diagnostic_log_state().level, LogSeverity::debug);
    ASSERT_TRUE(diagnostic_log_state().verbose_until);
    now += std::chrono::minutes(4);
    set_diagnostic_log_verbose(true);
    now += std::chrono::minutes(4);
    log_debug("still accepted");
    now += diagnostic_log_verbose_duration + std::chrono::seconds(1);
    log_debug("expired");
    const auto snapshot = snapshot_diagnostic_log();
    bool saw_expired = false;
    bool saw_accepted = false;
    for (const auto& entry : snapshot.entries) {
        if (entry.text.find("expired") != std::string::npos) saw_expired = true;
        if (entry.text.find("accepted") != std::string::npos) saw_accepted = true;
    }
    EXPECT_TRUE(saw_accepted);
    EXPECT_FALSE(saw_expired);
    EXPECT_EQ(diagnostic_log_state().level, LogSeverity::info);
    EXPECT_FALSE(diagnostic_log_state().verbose_until);
    set_diagnostic_log_verbose(false);
    EXPECT_EQ(diagnostic_log_state().level, LogSeverity::info);
}

TEST_F(LoggingTest, ConcurrentInsertSnapshotAndClear) {
    initialize_diagnostic_logging(log_file(), "off");
    std::atomic<int> writes{0};
    std::vector<std::thread> threads;
    threads.reserve(8);
    for (int worker = 0; worker < 8; ++worker) {
        threads.emplace_back([&, worker] {
            for (int index = 0; index < 50; ++index) {
                log_info("worker-" + std::to_string(worker) + "-" + std::to_string(index));
                writes.fetch_add(1);
                if (index % 10 == 0) (void)snapshot_diagnostic_log();
                if (index == 25 && worker == 0) clear_diagnostic_log();
            }
        });
    }
    for (auto& thread : threads) thread.join();
    const auto snapshot = snapshot_diagnostic_log();
    EXPECT_GE(writes.load(), 400);
    EXPECT_LE(snapshot.entries.size(), diagnostic_log_capacity);
    for (std::size_t index = 1; index < snapshot.entries.size(); ++index) {
        EXPECT_LT(snapshot.entries[index - 1].number, snapshot.entries[index].number);
    }
}

} // namespace
} // namespace cha
