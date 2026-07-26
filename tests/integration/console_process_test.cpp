#include "support/mock_http_server.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace cha {
namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;

void close_descriptor(int& descriptor) {
    if (descriptor != -1) {
        ::close(descriptor);
        descriptor = -1;
    }
}

void write_all(int descriptor, std::string_view text) {
    while (!text.empty()) {
        const ssize_t count =
            ::write(descriptor, text.data(), text.size());
        if (count == -1 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            throw std::runtime_error("Failed to write process input");
        }
        text.remove_prefix(static_cast<std::size_t>(count));
    }
}

void write_file(
    const std::filesystem::path& path,
    std::string_view contents) {
    std::ofstream output(path);
    if (!output) {
        throw std::runtime_error(
            "Failed to create process-test fixture file");
    }
    output << contents;
}

class TemporaryWorkspace {
public:
    TemporaryWorkspace()
        : path_(
            std::filesystem::temp_directory_path()
            / ("cha_console_process_"
               + std::to_string(::getpid()) + "_"
               + std::to_string(
                   std::chrono::steady_clock::now()
                       .time_since_epoch()
                       .count()))) {
        std::filesystem::create_directories(
            path_ / "forums" / "hall" / "personas" / "Ismael");
        std::filesystem::create_directories(
            path_ / "forums" / "hall" / "sessions");
        write_file(
            path_ / "forums" / "hall" / "config.toml",
            "display_name = \"The Hall\"\n");
        write_file(
            path_ / "forums" / "hall" / "personas" / "Ismael" / "config.toml",
            "display_name = \"Ismael\"\n");
        write_file(
            path_ / "forums" / "hall" / "personas" / "Ismael" / "SYSTEM.md",
            "You are a process test agent.\n");
        write_file(
            path_ / "forums" / "hall" / "USER.md",
            "Answer the user.\n");
    }

    ~TemporaryWorkspace() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    void point_at(int port) const {
        write_file(
            path_ / "forums" / "hall" / "personas" / "base_config.toml",
            "host = \"127.0.0.1\"\n"
            "port = " + std::to_string(port) + "\n"
            "https = false\n"
            "mode = \"net\"\n"
            "model = \"process-test-model\"\n"
            "stream = true\n"
            "api_key = \"process-test-key\"\n"
            "api_key_env = \"\"\n");
    }

    const std::filesystem::path& path() const {
        return path_;
    }

    bool has_session() const {
        const std::filesystem::path directory =
            path_ / "forums" / "hall" / "sessions";
        return std::filesystem::directory_iterator(directory)
            != std::filesystem::directory_iterator();
    }

private:
    std::filesystem::path path_;
};

struct ChildProcess {
    pid_t pid{-1};
    int input{-1};
    int output{-1};
    int errors{-1};
};

void make_nonblocking(int descriptor) {
    const int flags = ::fcntl(descriptor, F_GETFL);
    if (flags == -1
        || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == -1) {
        throw std::runtime_error(
            "Failed to make process-test pipe non-blocking");
    }
}

ChildProcess launch_console(
    const TemporaryWorkspace& workspace,
    const std::filesystem::path& input_path = {}) {
    int input_pipe[2]{-1, -1};
    int output_pipe[2]{};
    int error_pipe[2]{};
    if ((input_path.empty() && ::pipe(input_pipe) == -1)
        || ::pipe(output_pipe) == -1
        || ::pipe(error_pipe) == -1) {
        throw std::runtime_error("Failed to create process-test pipes");
    }

    const pid_t child = ::fork();
    if (child == -1) {
        throw std::runtime_error("Failed to fork console process");
    }
    if (child == 0) {
        if (input_path.empty()) {
            (void)::dup2(input_pipe[0], STDIN_FILENO);
        } else {
            const int input_file =
                ::open(input_path.c_str(), O_RDONLY);
            if (input_file == -1
                || ::dup2(input_file, STDIN_FILENO) == -1) {
                _exit(125);
            }
            ::close(input_file);
        }
        (void)::dup2(output_pipe[1], STDOUT_FILENO);
        (void)::dup2(error_pipe[1], STDERR_FILENO);
        close_descriptor(input_pipe[0]);
        close_descriptor(input_pipe[1]);
        ::close(output_pipe[0]);
        ::close(output_pipe[1]);
        ::close(error_pipe[0]);
        ::close(error_pipe[1]);

        sigset_t empty{};
#if defined(__APPLE__) && defined(__MACH__)
        (void)sigemptyset(&empty);
#else
        (void)::sigemptyset(&empty);
#endif
        (void)::pthread_sigmask(SIG_SETMASK, &empty, nullptr);
        if (::chdir(workspace.path().c_str()) == -1) {
            _exit(126);
        }
        const char* const arguments[]{
            CHA_CONSOLE_BINARY,
            "--forum",
            "hall",
            "--color=never",
            nullptr,
        };
        ::execv(
            CHA_CONSOLE_BINARY,
            const_cast<char* const*>(arguments));
        _exit(127);
    }

    close_descriptor(input_pipe[0]);
    ::close(output_pipe[1]);
    ::close(error_pipe[1]);
    make_nonblocking(output_pipe[0]);
    make_nonblocking(error_pipe[0]);
    return {
        child,
        input_pipe[1],
        output_pipe[0],
        error_pipe[0],
    };
}

void read_available(int& descriptor, std::string& destination) {
    if (descriptor == -1) {
        return;
    }
    std::array<char, 4096> buffer{};
    while (true) {
        const ssize_t count =
            ::read(descriptor, buffer.data(), buffer.size());
        if (count > 0) {
            destination.append(
                buffer.data(),
                static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0) {
            close_descriptor(descriptor);
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        close_descriptor(descriptor);
        throw std::runtime_error("Failed to read process output");
    }
}

struct ProcessResult {
    std::string output;
    std::string errors;
    int exit_code{};
    bool timed_out{};
};

ProcessResult run_to_completion(
    ChildProcess& process,
    std::chrono::milliseconds timeout = 5s,
    std::string initial_output = {}) {
    ProcessResult result{.output = std::move(initial_output)};
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int status{};
    bool reaped = false;

    while (!reaped || process.output != -1 || process.errors != -1) {
        read_available(process.output, result.output);
        read_available(process.errors, result.errors);
        if (!reaped) {
            const pid_t wait_result =
                ::waitpid(process.pid, &status, WNOHANG);
            if (wait_result == process.pid) {
                reaped = true;
            } else if (wait_result == -1) {
                throw std::runtime_error(
                    "Failed to wait for console process");
            }
        }
        if (reaped && process.output == -1 && process.errors == -1) {
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            if (!reaped) {
                (void)::kill(process.pid, SIGKILL);
                (void)::waitpid(process.pid, &status, 0);
                reaped = true;
            }
            break;
        }

        pollfd descriptors[2]{
            {process.output, POLLIN | POLLHUP | POLLERR, 0},
            {process.errors, POLLIN | POLLHUP | POLLERR, 0},
        };
        (void)::poll(descriptors, 2, 20);
    }

    close_descriptor(process.input);
    close_descriptor(process.output);
    close_descriptor(process.errors);
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }
    return result;
}

bool wait_for_session(
    const TemporaryWorkspace& workspace,
    std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (workspace.has_session()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return false;
}

bool wait_for_output(
    ChildProcess& process,
    std::string& output,
    std::string_view marker,
    std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        read_available(process.output, output);
        if (output.find(marker) != std::string::npos) {
            return true;
        }
        pollfd descriptor{
            process.output,
            POLLIN | POLLHUP | POLLERR,
            0,
        };
        (void)::poll(&descriptor, 1, 20);
    }
    return false;
}

std::string streamed_answer(std::string_view text) {
    const std::string body =
        "data: "
        + Json{
            {"choices", Json::array({
                Json{{"delta", Json{{"content", text}}}},
            })},
        }.dump()
        + "\n\ndata: [DONE]\n\n";
    return http_response("text/event-stream", body);
}

class SlowHttpServer {
public:
    SlowHttpServer() {
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == -1) {
            throw std::runtime_error("Failed to create slow server socket");
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
#if defined(__APPLE__) && defined(__MACH__)
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#else
        address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
#endif
        address.sin_port = 0;
        if (::bind(
                listener_,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == -1
            || ::listen(listener_, 1) == -1) {
            throw std::runtime_error("Failed to bind slow server socket");
        }
        socklen_t size = sizeof(address);
        if (::getsockname(
                listener_,
                reinterpret_cast<sockaddr*>(&address),
                &size) == -1) {
            throw std::runtime_error("Failed to inspect slow server socket");
        }
#if defined(__APPLE__) && defined(__MACH__)
        port_ = static_cast<int>(ntohs(address.sin_port));
#else
        port_ = static_cast<int>(::ntohs(address.sin_port));
#endif
    }

    ~SlowHttpServer() {
        join();
        close_descriptor(listener_);
    }

    int port() const {
        return port_;
    }

    void start() {
        thread_ = std::thread([this] {
            pollfd ready{listener_, POLLIN, 0};
            if (::poll(&ready, 1, 5000) != 1) {
                return;
            }
            int client = ::accept(listener_, nullptr, nullptr);
            if (client == -1) {
                return;
            }
            std::array<char, 4096> request{};
            (void)::recv(client, request.data(), request.size(), 0);

            const std::string first =
                "data: {\"choices\":[{\"delta\":{\"content\":"
                "\"Partial answer\"}}]}\n\n";
            const std::string last = "data: [DONE]\n\n";
            const std::size_t body_size = first.size() + last.size();
            const std::string headers =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "Content-Length: " + std::to_string(body_size)
                + "\r\nConnection: close\r\n\r\n";
            (void)::send(
                client,
                headers.data(),
                headers.size(),
                MSG_NOSIGNAL);
            (void)::send(
                client,
                first.data(),
                first.size(),
                MSG_NOSIGNAL);
            std::this_thread::sleep_for(500ms);
            (void)::send(
                client,
                last.data(),
                last.size(),
                MSG_NOSIGNAL);
            ::shutdown(client, SHUT_RDWR);
            ::close(client);
        });
    }

    void join() {
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    int listener_{-1};
    int port_{};
    std::thread thread_;
};

TEST(ConsoleProcess, PromptThenImmediateEofCompletes) {
    MockHttpServer server({streamed_answer("Complete answer")});
    TemporaryWorkspace workspace;
    workspace.point_at(server.port());
    server.start();
    ChildProcess process = launch_console(workspace);
    write_all(process.input, "hello\n");
    close_descriptor(process.input);

    const ProcessResult result = run_to_completion(process);
    server.join();
    EXPECT_FALSE(result.timed_out);
    EXPECT_EQ(result.exit_code, 0) << result.errors;
    EXPECT_NE(result.output.find("Complete answer"), std::string::npos);
}

TEST(ConsoleProcess, RedirectedRegularFileCompletes) {
    MockHttpServer server({streamed_answer("File answer")});
    TemporaryWorkspace workspace;
    workspace.point_at(server.port());
    const std::filesystem::path input_path =
        workspace.path() / "input.txt";
    write_file(input_path, "hello from file\n");
    server.start();
    ChildProcess process =
        launch_console(workspace, input_path);

    const ProcessResult result = run_to_completion(process);
    server.join();
    EXPECT_FALSE(result.timed_out);
    EXPECT_EQ(result.exit_code, 0) << result.errors;
    EXPECT_NE(result.output.find("File answer"), std::string::npos);
}

TEST(ConsoleProcess, SeveralPromptsInOneWriteRunInOrder) {
    MockHttpServer server({
        streamed_answer("First answer"),
        streamed_answer("Second answer"),
        streamed_answer("Third answer"),
    });
    TemporaryWorkspace workspace;
    workspace.point_at(server.port());
    server.start();
    ChildProcess process = launch_console(workspace);
    write_all(process.input, "one\ntwo\nthree\n");
    close_descriptor(process.input);

    const ProcessResult result = run_to_completion(process);
    server.join();
    ASSERT_FALSE(result.timed_out);
    ASSERT_EQ(result.exit_code, 0) << result.errors;
    const std::size_t first = result.output.find("First answer");
    const std::size_t second = result.output.find("Second answer");
    const std::size_t third = result.output.find("Third answer");
    ASSERT_NE(first, std::string::npos);
    ASSERT_NE(second, std::string::npos);
    ASSERT_NE(third, std::string::npos);
    EXPECT_LT(first, second);
    EXPECT_LT(second, third);
}

TEST(ConsoleProcess, InterruptWhileGeneratingKeepsPartialOutputAndSurvives) {
    SlowHttpServer server;
    TemporaryWorkspace workspace;
    workspace.point_at(server.port());
    server.start();
    ChildProcess process = launch_console(workspace);
    write_all(process.input, "slow question\n");

    std::string output;
    ASSERT_TRUE(wait_for_output(process, output, "Partial answer"));
    ASSERT_EQ(::kill(process.pid, SIGINT), 0);
    close_descriptor(process.input);
    const ProcessResult result =
        run_to_completion(process, 5s, std::move(output));
    server.join();

    EXPECT_FALSE(result.timed_out);
    EXPECT_EQ(result.exit_code, 0) << result.errors;
    EXPECT_NE(result.output.find("Partial answer"), std::string::npos);
    EXPECT_NE(result.errors.find("Stopping generation"), std::string::npos);
}

TEST(ConsoleProcess, InterruptWhileIdleExitsPromptly) {
    TemporaryWorkspace workspace;
    workspace.point_at(9);
    ChildProcess process = launch_console(workspace);
    ASSERT_TRUE(wait_for_session(workspace));
    ASSERT_EQ(::kill(process.pid, SIGINT), 0);

    const ProcessResult result = run_to_completion(process, 2s);
    EXPECT_FALSE(result.timed_out);
    EXPECT_EQ(result.exit_code, 0) << result.errors;
}

TEST(ConsoleProcess, EscapeInjectionIsNeutralized) {
    const std::string injected = "\x1b]0;pwned\x07";
    MockHttpServer server({streamed_answer(injected)});
    TemporaryWorkspace workspace;
    workspace.point_at(server.port());
    server.start();
    ChildProcess process = launch_console(workspace);
    write_all(process.input, "hello\n");
    close_descriptor(process.input);

    const ProcessResult result = run_to_completion(process);
    server.join();
    ASSERT_EQ(result.exit_code, 0) << result.errors;
    EXPECT_EQ(result.output.find('\x1b'), std::string::npos);
    EXPECT_NE(result.output.find("^[]0;pwned^G"), std::string::npos);
}

TEST(ConsoleProcess, ClosedStdoutReportsFailureInsteadOfSigpipe) {
    TemporaryWorkspace workspace;
    workspace.point_at(9);
    ChildProcess process = launch_console(workspace);
    close_descriptor(process.output);
    write_all(process.input, "hello\n");
    close_descriptor(process.input);

    const ProcessResult result = run_to_completion(process);
    EXPECT_FALSE(result.timed_out);
    EXPECT_EQ(result.exit_code, 1) << result.errors;
    EXPECT_NE(
        result.errors.find("Failed to write console transcript"),
        std::string::npos);
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
        ::execlp(
            "otool",
            "otool",
            "-L",
            CHA_CONSOLE_BINARY,
            static_cast<char*>(nullptr));
#else
        ::execlp(
            "ldd",
            "ldd",
            CHA_CONSOLE_BINARY,
            static_cast<char*>(nullptr));
#endif
        _exit(127);
    }
    ::close(output_pipe[1]);
    std::string output;
    std::array<char, 4096> buffer{};
    while (true) {
        const ssize_t count =
            ::read(output_pipe[0], buffer.data(), buffer.size());
        if (count > 0) {
            output.append(
                buffer.data(),
                static_cast<std::size_t>(count));
        } else if (count == -1 && errno == EINTR) {
            continue;
        } else {
            break;
        }
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
