#include "util/logging.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace cha {
namespace {

class LoggingTest : public testing::Test {
protected:
    void SetUp() override {
        directory_ = std::filesystem::temp_directory_path()
            / ("cha_logging_"
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

TEST_F(LoggingTest, IgnoresRepeatedInitialization) {
    const std::filesystem::path first = log_file("first.log");
    const std::filesystem::path second = log_file("second.log");

    initialize_diagnostic_logging(first, "info");
    initialize_diagnostic_logging(second, "info");
    log_info("written once");

    EXPECT_NE(contents(first).find("written once"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(second));
}

} // namespace
} // namespace cha
