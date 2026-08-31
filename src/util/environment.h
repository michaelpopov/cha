#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

// Updates one process environment variable. On Windows this uses _putenv_s;
// on POSIX it uses setenv. Existing values are preserved when overwrite is
// false.
bool set_environment_variable(
    std::string_view name,
    std::string_view value,
    bool overwrite = true);

// Removes one process environment variable.
bool unset_environment_variable(std::string_view name);

struct DotenvEntry {
    std::string name;
    std::string value;
};

// Parses a dotenv file without modifying the process environment. A missing
// file yields an empty list.
std::vector<DotenvEntry> parse_dotenv(const std::filesystem::path& path);

// Applies parsed dotenv entries without replacing inherited variables.
void apply_dotenv(
    const std::vector<DotenvEntry>& entries,
    const std::filesystem::path& source = {});

// Loads variables from a dotenv file without replacing values inherited by the process.
void load_dotenv(const std::filesystem::path& path = ".env");

// Installs dotenv entries that are absent from the inherited environment and
// removes those insertions when destroyed, including during exception unwind.
class ScopedEnvironmentOverlay {
public:
    explicit ScopedEnvironmentOverlay(const std::vector<DotenvEntry>& entries);
    ~ScopedEnvironmentOverlay();

    ScopedEnvironmentOverlay(const ScopedEnvironmentOverlay&) = delete;
    ScopedEnvironmentOverlay& operator=(const ScopedEnvironmentOverlay&) = delete;
    ScopedEnvironmentOverlay(ScopedEnvironmentOverlay&&) = delete;
    ScopedEnvironmentOverlay& operator=(ScopedEnvironmentOverlay&&) = delete;

private:
    void restore() noexcept;

    std::vector<std::string> inserted_;
};

} // namespace cha
