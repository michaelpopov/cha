#include "util/environment.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace cha {
namespace {

// Temporarily sets one environment variable so dotenv tests do not affect each other.
class ScopedEnvironmentVariable {
public:
    explicit ScopedEnvironmentVariable(std::string name) : name_(std::move(name)) {
        if (const char* value = std::getenv(name_.c_str())) {
            previous_value_ = value;
        }
    }

    ~ScopedEnvironmentVariable() {
        if (previous_value_) {
            (void)set_environment_variable(name_, *previous_value_);
        } else {
            (void)unset_environment_variable(name_);
        }
    }

private:
    std::string name_;
    std::optional<std::string> previous_value_;
};

std::filesystem::path unique_dotenv_path() {
    return std::filesystem::temp_directory_path()
        / ("cha_dotenv_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
}

TEST(Environment, MissingDotenvIsIgnored) {
    EXPECT_NO_THROW(load_dotenv(unique_dotenv_path()));
}

TEST(Environment, LoadsValuesAndSkipsComments) {
    const auto path = unique_dotenv_path();
    {
        std::ofstream file(path);
        file << "\n# Local settings\nCHA_DOTENV_ONE = one\nCHA_DOTENV_TWO=\"two words\"\nCHA_DOTENV_THREE='three'\n";
    }
    ScopedEnvironmentVariable first("CHA_DOTENV_ONE");
    ScopedEnvironmentVariable second("CHA_DOTENV_TWO");
    ScopedEnvironmentVariable third("CHA_DOTENV_THREE");
    (void)unset_environment_variable("CHA_DOTENV_ONE");
    (void)unset_environment_variable("CHA_DOTENV_TWO");
    (void)unset_environment_variable("CHA_DOTENV_THREE");

    load_dotenv(path);

    EXPECT_STREQ(std::getenv("CHA_DOTENV_ONE"), "one");
    EXPECT_STREQ(std::getenv("CHA_DOTENV_TWO"), "two words");
    EXPECT_STREQ(std::getenv("CHA_DOTENV_THREE"), "three");
    std::filesystem::remove(path);
}

TEST(Environment, DoesNotOverrideExistingValues) {
    const auto path = unique_dotenv_path();
    {
        std::ofstream file(path);
        file << "CHA_DOTENV_EXISTING=file-value\n";
    }
    ScopedEnvironmentVariable variable("CHA_DOTENV_EXISTING");
    ASSERT_TRUE(
        set_environment_variable(
            "CHA_DOTENV_EXISTING",
            "process-value"));

    load_dotenv(path);

    EXPECT_STREQ(std::getenv("CHA_DOTENV_EXISTING"), "process-value");
    std::filesystem::remove(path);
}

TEST(Environment, RejectsInvalidEntries) {
    const auto path = unique_dotenv_path();
    {
        std::ofstream file(path);
        file << "not a valid entry\n";
    }

    EXPECT_THROW(load_dotenv(path), std::runtime_error);
    std::filesystem::remove(path);
}

TEST(Environment, ParseDotenvDoesNotModifyTheProcessEnvironment) {
    const auto path = unique_dotenv_path();
    {
        std::ofstream file(path);
        file << "CHA_DOTENV_PARSED=from-file\n"
                "CHA_DOTENV_PARSED=second\n";
    }
    ScopedEnvironmentVariable variable("CHA_DOTENV_PARSED");
    (void)unset_environment_variable("CHA_DOTENV_PARSED");

    const std::vector<DotenvEntry> entries = parse_dotenv(path);
    ASSERT_EQ(entries.size(), 2U);
    EXPECT_EQ(entries[0].name, "CHA_DOTENV_PARSED");
    EXPECT_EQ(entries[0].value, "from-file");
    EXPECT_EQ(entries[1].value, "second");
    EXPECT_EQ(std::getenv("CHA_DOTENV_PARSED"), nullptr);

    apply_dotenv(entries, path);
    EXPECT_STREQ(std::getenv("CHA_DOTENV_PARSED"), "from-file");
    std::filesystem::remove(path);
}

TEST(Environment, OverlayInstallsAbsentNamesAndCleansUp) {
    ScopedEnvironmentVariable inherited("CHA_DOTENV_INHERITED");
    ScopedEnvironmentVariable inserted("CHA_DOTENV_INSERTED");
    ScopedEnvironmentVariable unrelated("CHA_DOTENV_UNRELATED");
    ASSERT_TRUE(set_environment_variable("CHA_DOTENV_INHERITED", "process"));
    ASSERT_TRUE(set_environment_variable("CHA_DOTENV_UNRELATED", "keep"));
    (void)unset_environment_variable("CHA_DOTENV_INSERTED");

    const std::vector<DotenvEntry> entries{
        {"CHA_DOTENV_INHERITED", "file"},
        {"CHA_DOTENV_INSERTED", "added"},
        {"CHA_DOTENV_INSERTED", "later"},
    };
    {
        ScopedEnvironmentOverlay overlay(entries);
        EXPECT_STREQ(std::getenv("CHA_DOTENV_INHERITED"), "process");
        EXPECT_STREQ(std::getenv("CHA_DOTENV_INSERTED"), "added");
        EXPECT_STREQ(std::getenv("CHA_DOTENV_UNRELATED"), "keep");
    }
    EXPECT_STREQ(std::getenv("CHA_DOTENV_INHERITED"), "process");
    EXPECT_EQ(std::getenv("CHA_DOTENV_INSERTED"), nullptr);
    EXPECT_STREQ(std::getenv("CHA_DOTENV_UNRELATED"), "keep");
}

TEST(Environment, OverlayPreservesInheritedEmptyValues) {
    ScopedEnvironmentVariable variable("CHA_DOTENV_EMPTY_INHERITED");
    ASSERT_TRUE(set_environment_variable("CHA_DOTENV_EMPTY_INHERITED", ""));
    if (std::getenv("CHA_DOTENV_EMPTY_INHERITED") == nullptr) {
        GTEST_SKIP() << "this platform does not retain empty environment values";
    }

    const std::vector<DotenvEntry> entries{
        {"CHA_DOTENV_EMPTY_INHERITED", "from-file"}};
    {
        ScopedEnvironmentOverlay overlay(entries);
        EXPECT_STREQ(std::getenv("CHA_DOTENV_EMPTY_INHERITED"), "");
    }
    EXPECT_STREQ(std::getenv("CHA_DOTENV_EMPTY_INHERITED"), "");
}

TEST(Environment, OverlayCleansUpWhenAnExceptionEscapes) {
    ScopedEnvironmentVariable variable("CHA_DOTENV_SCOPED");
    (void)unset_environment_variable("CHA_DOTENV_SCOPED");
    try {
        ScopedEnvironmentOverlay overlay({{"CHA_DOTENV_SCOPED", "temp"}});
        EXPECT_STREQ(std::getenv("CHA_DOTENV_SCOPED"), "temp");
        throw std::runtime_error("forced");
    } catch (const std::runtime_error& error) {
        EXPECT_STREQ(error.what(), "forced");
    }
    EXPECT_EQ(std::getenv("CHA_DOTENV_SCOPED"), nullptr);
}

} // namespace
} // namespace cha
