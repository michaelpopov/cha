#pragma once

#include <filesystem>
#include <string_view>

namespace cha::test {

// Small reusable workspace fixture for web tests. The default persona uses the
// deterministic in-process test provider mode, so callers never need a live
// provider merely to construct a workspace.
class TestWorkspace {
public:
    TestWorkspace();
    ~TestWorkspace();
    TestWorkspace(const TestWorkspace&) = delete;
    TestWorkspace& operator=(const TestWorkspace&) = delete;

    const std::filesystem::path& root() const noexcept { return root_; }
    void write_app_config(
        int port,
        std::string_view log_level = "off") const;
    void write_persona_config(std::string_view contents) const;

private:
    std::filesystem::path root_;
};

} // namespace cha::test
