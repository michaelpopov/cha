#pragma once

#include <chrono>
#include <filesystem>
#include <string>

#include <sys/types.h>

namespace cha::test {

struct ProcessExit {
    int exit_code{};
    bool timed_out{};
};

// Owns a real chaweb child from fork/exec through reap. The child receives
// dedicated stdout/stderr pipes and a clean signal mask; destruction always
// SIGKILLs and reaps a still-running child, so an ASSERT_* cannot orphan the
// server or leave ctest's capture descriptors open.
class WebServerProcess {
public:
    WebServerProcess(const std::filesystem::path& workspace, int port);
    // The two roots are independent. An empty application root means the
    // fixture is using one directory for both, which most tests do.
    WebServerProcess(
        const std::filesystem::path& application_root,
        const std::filesystem::path& workspace,
        int port);
    ~WebServerProcess();
    WebServerProcess(const WebServerProcess&) = delete;
    WebServerProcess& operator=(const WebServerProcess&) = delete;

    [[nodiscard]] bool wait_until_ready(
        std::chrono::milliseconds timeout = std::chrono::seconds(3));
    [[nodiscard]] ProcessExit stop(
        int signal,
        std::chrono::milliseconds timeout = std::chrono::seconds(3));

    [[nodiscard]] const std::string& output() const noexcept { return output_; }
    [[nodiscard]] const std::string& errors() const noexcept { return errors_; }

private:
    void drain_output() noexcept;
    void kill_and_reap() noexcept;

    pid_t pid_{-1};
    int output_fd_{-1};
    int error_fd_{-1};
    int port_{};
    std::string output_;
    std::string errors_;
};

[[nodiscard]] int reserve_loopback_port();

} // namespace cha::test
