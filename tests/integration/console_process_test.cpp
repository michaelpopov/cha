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
    while (true) {
        const pid_t waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child) break;
        if (waited == -1 && errno == EINTR) continue;
        if (waited == -1) {
            (void)::kill(child, SIGKILL);
            throw std::runtime_error("Failed to wait for console process");
        }
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

TEST(ConsoleProcess, SeveralRedirectedPromptsRunInOrder) {
    test::TestWorkspace workspace;
    const ProcessResult result = run_console(workspace.root(), "one\ntwo\nthree\n");
    ASSERT_EQ(result.exit_code, 0) << result.errors;
    const std::size_t first = result.output.find("[Guest] one");
    const std::size_t second = result.output.find("[Guest] two");
    const std::size_t third = result.output.find("[Guest] three");
    ASSERT_NE(first, std::string::npos);
    ASSERT_NE(second, std::string::npos);
    ASSERT_NE(third, std::string::npos);
    EXPECT_LT(first, second);
    EXPECT_LT(second, third);
}

TEST(ConsoleProcess, EscapeInjectionInTranscriptIsNeutralized) {
    test::TestWorkspace workspace;
    const ProcessResult result = run_console(
        workspace.root(), std::string("hello ") + "\x1b]0;pwned\x07\n");
    ASSERT_EQ(result.exit_code, 0) << result.errors;
    EXPECT_EQ(result.output.find('\x1b'), std::string::npos);
    EXPECT_NE(result.output.find("^[]0;pwned^G"), std::string::npos);
}

TEST(ConsoleProcess, ConsoleBinaryDoesNotLinkNcurses) {
    int output_pipe[2]{};
    ASSERT_EQ(::pipe(output_pipe), 0);
    const pid_t child = ::fork();
    ASSERT_NE(child, -1);
    if (child == 0) {
        (void)::dup2(output_pipe[1], STDOUT_FILENO);
        (void)::dup2(output_pipe[1], STDERR_FILENO);
        ::close(output_pipe[0]);
        ::close(output_pipe[1]);
#if defined(__APPLE__) && defined(__MACH__)
        ::execlp("otool", "otool", "-L", CHA_CONSOLE_BINARY,
            static_cast<char*>(nullptr));
#else
        ::execlp("ldd", "ldd", CHA_CONSOLE_BINARY,
            static_cast<char*>(nullptr));
#endif
        _exit(127);
    }
    ::close(output_pipe[1]);
    std::string output;
    std::array<char, 4096> buffer{};
    while (true) {
        const ssize_t count = ::read(output_pipe[0], buffer.data(), buffer.size());
        if (count > 0) output.append(buffer.data(), static_cast<std::size_t>(count));
        else if (count == -1 && errno == EINTR) continue;
        else break;
    }
    ::close(output_pipe[0]);
    int status{};
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0) << output;
    EXPECT_EQ(output.find("ncurses"), std::string::npos) << output;
}

} // namespace
} // namespace cha
