#include "support/test_workspace.h"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <stdexcept>
#include <sys/wait.h>
#include <thread>
#include <pthread.h>
#include <unistd.h>

namespace cha {
namespace {

struct ProcessResult {
    std::string output;
    std::string errors;
    int exit_code{};
};

ProcessResult run_console(
    const std::filesystem::path& workspace,
    std::string_view input,
    bool check = false) {
    const std::filesystem::path input_path = workspace / "console-input.txt";
    const std::filesystem::path output_path = workspace / "console-output.txt";
    const std::filesystem::path error_path = workspace / "console-errors.txt";
    std::ofstream(input_path) << input;
    const pid_t child = ::fork();
    if (child == 0) {
        if (::chdir(workspace.c_str()) != 0) _exit(125);
        (void)::freopen(input_path.c_str(), "r", stdin);
        (void)::freopen(output_path.c_str(), "w", stdout);
        (void)::freopen(error_path.c_str(), "w", stderr);
        sigset_t empty{};
        (void)::sigemptyset(&empty);
        (void)::pthread_sigmask(SIG_SETMASK, &empty, nullptr);
        if (check) {
            ::execl(CHA_CONSOLE_BINARY, CHA_CONSOLE_BINARY, "--check", static_cast<char*>(nullptr));
        } else {
            ::execl(CHA_CONSOLE_BINARY, CHA_CONSOLE_BINARY, "--color=never", static_cast<char*>(nullptr));
        }
        _exit(127);
    }
    if (child == -1) {
        throw std::runtime_error("Failed to fork console process");
    }
    int status{};
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(5);
    while (::waitpid(child, &status, WNOHANG) == 0) {
        if (std::chrono::steady_clock::now() >= deadline) {
            (void)::kill(child, SIGKILL);
            (void)::waitpid(child, &status, 0);
            throw std::runtime_error("Console process timed out");
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::ifstream output(output_path);
    std::string text((std::istreambuf_iterator<char>(output)), {});
    std::ifstream errors(error_path);
    std::string error_text((std::istreambuf_iterator<char>(errors)), {});
    return {std::move(text), std::move(error_text),
            WIFEXITED(status) ? WEXITSTATUS(status) : 128};
}

TEST(ConsoleProcess, StartsWithoutSelectionArgumentsAndUsesGuest) {
    test::TestWorkspace workspace;
    const ProcessResult result = run_console(workspace.root(), "hello\n");
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_NE(result.output.find("[Guest] hello"), std::string::npos);
    EXPECT_EQ(result.output.find("builtin-guest"), std::string::npos);
    EXPECT_TRUE(result.errors.empty());
}

TEST(ConsoleProcess, NavigationNoticeStaysOffTheTranscriptStream) {
    test::TestWorkspace workspace;
    const ProcessResult result = run_console(
        workspace.root(), "/create \"The Lobby\" \"Process session\"\n");
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_EQ(result.output.find("Opened session"), std::string::npos);
    EXPECT_NE(result.errors.find("Opened session 'Process session' in forum 'The Lobby'"), std::string::npos);
    EXPECT_EQ(result.errors.find("builtin-guest"), std::string::npos);
}

TEST(ConsoleProcess, WholeWorkspaceCheckNeedsNoProviderCredential) {
    test::TestWorkspace workspace;
    const ProcessResult result = run_console(workspace.root(), "", true);
    EXPECT_EQ(result.exit_code, 0);
    EXPECT_EQ(result.output, "Workspace is valid.\n");
}

} // namespace
} // namespace cha
