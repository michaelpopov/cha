#include "environment.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>

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
            (void)::setenv(name_.c_str(), previous_value_->c_str(), 1);
        } else {
            (void)::unsetenv(name_.c_str());
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
    (void)::unsetenv("CHA_DOTENV_ONE");
    (void)::unsetenv("CHA_DOTENV_TWO");
    (void)::unsetenv("CHA_DOTENV_THREE");

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
    ASSERT_EQ(::setenv("CHA_DOTENV_EXISTING", "process-value", 1), 0);

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

} // namespace
} // namespace cha
