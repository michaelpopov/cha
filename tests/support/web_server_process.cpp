#include "support/web_server_process.h"

#include <httplib.h>

#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include <array>

namespace cha::test {
namespace {

// The address every child binds, connects back on, and announces on stdout.
constexpr const char* loopback_host = "127.0.0.1";

void close_descriptor(int& descriptor) noexcept {
    if (descriptor == -1) return;
    (void)::close(descriptor);
    descriptor = -1;
}

void make_nonblocking(int descriptor) {
    const int flags = ::fcntl(descriptor, F_GETFL);
    if (flags == -1
        || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == -1) {
        throw std::runtime_error("Failed to configure web process-test pipe");
    }
}

void read_available(int& descriptor, std::string& destination) noexcept {
    if (descriptor == -1) return;
    std::array<char, 4096> buffer{};
    while (true) {
        const ssize_t count = ::read(descriptor, buffer.data(), buffer.size());
        if (count > 0) {
            destination.append(buffer.data(), static_cast<std::size_t>(count));
            continue;
        }
        if (count == 0) {
            close_descriptor(descriptor);
            return;
        }
        if (errno == EINTR) continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return;
        close_descriptor(descriptor);
        return;
    }
}

void reset_child_signals() noexcept {
    sigset_t empty{};
    (void)::sigemptyset(&empty);
    (void)::pthread_sigmask(SIG_SETMASK, &empty, nullptr);

    struct sigaction action {};
    action.sa_handler = SIG_DFL;
    (void)::sigemptyset(&action.sa_mask);
    (void)::sigaction(SIGINT, &action, nullptr);
    (void)::sigaction(SIGTERM, &action, nullptr);
}

} // namespace

int reserve_loopback_port() {
    const int socket_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (socket_fd < 0) return 0;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(
            socket_fd,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) != 0) {
        (void)::close(socket_fd);
        return 0;
    }
    socklen_t size = sizeof(address);
    if (::getsockname(
            socket_fd,
            reinterpret_cast<sockaddr*>(&address),
            &size) != 0) {
        (void)::close(socket_fd);
        return 0;
    }
    (void)::close(socket_fd);
    return ntohs(address.sin_port);
}

WebServerProcess::WebServerProcess(
    const std::filesystem::path& workspace,
    int port)
    : WebServerProcess(workspace, workspace, port) {}

WebServerProcess::WebServerProcess(
    const std::filesystem::path& application_root,
    const std::filesystem::path& workspace,
    int port)
    : port_(port) {
    const std::string root_text = application_root.string();
    const std::string workspace_text = workspace.string();
    const std::string port_text = std::to_string(port);
    int output_pipe[2]{-1, -1};
    int error_pipe[2]{-1, -1};
    if (::pipe(output_pipe) == -1 || ::pipe(error_pipe) == -1) {
        close_descriptor(output_pipe[0]);
        close_descriptor(output_pipe[1]);
        close_descriptor(error_pipe[0]);
        close_descriptor(error_pipe[1]);
        throw std::runtime_error("Failed to create web process-test pipes");
    }
    try {
        make_nonblocking(output_pipe[0]);
        make_nonblocking(error_pipe[0]);
    } catch (...) {
        close_descriptor(output_pipe[0]);
        close_descriptor(output_pipe[1]);
        close_descriptor(error_pipe[0]);
        close_descriptor(error_pipe[1]);
        throw;
    }

    const pid_t child = ::fork();
    if (child == -1) {
        close_descriptor(output_pipe[0]);
        close_descriptor(output_pipe[1]);
        close_descriptor(error_pipe[0]);
        close_descriptor(error_pipe[1]);
        throw std::runtime_error("Failed to fork web server process");
    }
    if (child == 0) {
        (void)::dup2(output_pipe[1], STDOUT_FILENO);
        (void)::dup2(error_pipe[1], STDERR_FILENO);
        close_descriptor(output_pipe[0]);
        close_descriptor(output_pipe[1]);
        close_descriptor(error_pipe[0]);
        close_descriptor(error_pipe[1]);
        reset_child_signals();
        if (::chdir("/") != 0) _exit(126);
        ::execl(
            CHA_WEB_BINARY,
            CHA_WEB_BINARY,
            "--root",
            root_text.c_str(),
            "--workspace",
            workspace_text.c_str(),
            "--host",
            loopback_host,
            "--port",
            port_text.c_str(),
            static_cast<char*>(nullptr));
        _exit(127);
    }

    pid_ = child;
    output_fd_ = output_pipe[0];
    error_fd_ = error_pipe[0];
    close_descriptor(output_pipe[1]);
    close_descriptor(error_pipe[1]);
}

WebServerProcess::~WebServerProcess() {
    kill_and_reap();
    drain_output();
    close_descriptor(output_fd_);
    close_descriptor(error_fd_);
}

bool WebServerProcess::wait_until_ready(std::chrono::milliseconds timeout) {
    // The child answers /health from its listener thread before main announces
    // the address, and that announcement then has to reach this process through
    // the stdout pipe. Serving alone would let a caller read output() before
    // either happened, so readiness means both.
    const std::string announcement =
        "CHA ready at " + std::string(loopback_host) + ':'
        + std::to_string(port_);
    httplib::Client client(loopback_host, port_);
    client.set_connection_timeout(0, 100000);
    client.set_read_timeout(0, 100000);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    bool serving = false;
    while (std::chrono::steady_clock::now() < deadline) {
        drain_output();
        int status{};
        const pid_t waited = ::waitpid(pid_, &status, WNOHANG);
        if (waited == pid_) {
            pid_ = -1;
            drain_output();
            return false;
        }
        if (waited == -1) {
            pid_ = -1;
            return false;
        }
        if (!serving) {
            const httplib::Result health = client.Get("/health");
            serving = health && health->status == 200;
        }
        if (serving && output_.find(announcement) != std::string::npos) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

ProcessExit WebServerProcess::stop(
    int signal,
    std::chrono::milliseconds timeout) {
    ProcessExit result;
    if (pid_ == -1) return result;
    if (::kill(pid_, signal) == -1 && errno != ESRCH) {
        result.exit_code = -1;
        return result;
    }

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    int status{};
    while (std::chrono::steady_clock::now() < deadline) {
        drain_output();
        const pid_t waited = ::waitpid(pid_, &status, WNOHANG);
        if (waited == pid_) {
            pid_ = -1;
            break;
        }
        if (waited == -1) {
            pid_ = -1;
            result.exit_code = -1;
            return result;
        }
        pollfd descriptors[2]{
            {output_fd_, POLLIN | POLLHUP | POLLERR, 0},
            {error_fd_, POLLIN | POLLHUP | POLLERR, 0},
        };
        (void)::poll(descriptors, 2, 20);
    }
    if (pid_ != -1) {
        result.timed_out = true;
        (void)::kill(pid_, SIGKILL);
        (void)::waitpid(pid_, &status, 0);
        pid_ = -1;
    }
    drain_output();
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }
    return result;
}

void WebServerProcess::drain_output() noexcept {
    read_available(output_fd_, output_);
    read_available(error_fd_, errors_);
}

void WebServerProcess::kill_and_reap() noexcept {
    if (pid_ == -1) return;
    (void)::kill(pid_, SIGKILL);
    while (::waitpid(pid_, nullptr, 0) == -1 && errno == EINTR) {
    }
    pid_ = -1;
}

} // namespace cha::test
