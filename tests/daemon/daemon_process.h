#pragma once

#include "daemon/scgi.h"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef CHA_DAEMON_EXECUTABLE
#error "CHA_DAEMON_EXECUTABLE must name the cha-daemon executable"
#endif

namespace cha::test {

struct DaemonSpawn {
    std::filesystem::path executable{CHA_DAEMON_EXECUTABLE};
    std::filesystem::path config_directory;
    bool set_listen_pid{true};
    bool set_listen_fds{true};
    std::string listen_fds{"1"};
    bool pass_listen_socket{true};
    bool equals_config_option{true};
    std::optional<std::string> listen_pid;
    std::vector<std::string> extra_arguments;
};

class DaemonProcess {
public:
    explicit DaemonProcess(DaemonSpawn spawn) {
        int err[2]{};
        if (::pipe(err) != 0) {
            throw std::system_error(
                errno, std::generic_category(), "Failed to create stderr pipe");
        }
        stderr_ = daemon::UniqueFd(err[0]);
        daemon::UniqueFd stderr_write(err[1]);
        set_cloexec(stderr_.get());
        set_nonblock(stderr_.get());

        daemon::UniqueFd listen;
        if (spawn.pass_listen_socket) {
            listen = create_listen_socket();
        } else {
            const int null_fd = ::open("/dev/null", O_RDWR | O_CLOEXEC);
            if (null_fd < 0) {
                throw std::system_error(
                    errno, std::generic_category(), "Failed to open /dev/null");
            }
            listen = daemon::UniqueFd(null_fd);
        }

        const pid_t pid = ::fork();
        if (pid < 0) {
            throw std::system_error(
                errno, std::generic_category(), "Failed to fork cha-daemon");
        }
        if (pid == 0) {
            const int listen_raw = listen.release();
            const int err_write_raw = stderr_write.release();
            const int err_read_raw = stderr_.release();
            if (::dup2(listen_raw, 3) < 0) _exit(127);
            if (::dup2(err_write_raw, STDERR_FILENO) < 0) _exit(127);
            if (listen_raw != 3 && listen_raw != STDERR_FILENO) {
                ::close(listen_raw);
            }
            if (err_write_raw != 3 && err_write_raw != STDERR_FILENO) {
                ::close(err_write_raw);
            }
            if (err_read_raw != 3 && err_read_raw != STDERR_FILENO) {
                ::close(err_read_raw);
            }

            if (spawn.set_listen_pid) {
                const std::string pid_text = spawn.listen_pid
                    ? *spawn.listen_pid
                    : std::to_string(::getpid());
                if (::setenv("LISTEN_PID", pid_text.c_str(), 1) != 0) _exit(127);
            } else {
                ::unsetenv("LISTEN_PID");
            }
            if (spawn.set_listen_fds) {
                if (::setenv("LISTEN_FDS", spawn.listen_fds.c_str(), 1) != 0) {
                    _exit(127);
                }
            } else {
                ::unsetenv("LISTEN_FDS");
            }

            const std::string config = spawn.config_directory.string();
            std::vector<char*> arguments;
            arguments.push_back(const_cast<char*>(spawn.executable.c_str()));
            char config_flag[] = "--config";
            std::string joined;
            if (spawn.equals_config_option) {
                joined = std::string("--config=") + config;
                arguments.push_back(joined.data());
            } else {
                arguments.push_back(config_flag);
                arguments.push_back(const_cast<char*>(config.c_str()));
            }
            for (auto& argument : spawn.extra_arguments) arguments.push_back(argument.data());
            arguments.push_back(nullptr);
            ::execv(spawn.executable.c_str(), arguments.data());
            _exit(127);
        }

        pid_ = pid;
        stderr_write.close();
        listen.close();
    }

    DaemonProcess(const DaemonProcess&) = delete;
    DaemonProcess& operator=(const DaemonProcess&) = delete;

    ~DaemonProcess() {
        if (pid_ != -1) {
            (void)::kill(pid_, SIGKILL);
            int status = 0;
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
            }
        }
        if (!socket_path_.empty()) {
            ::unlink(socket_path_.c_str());
        }
    }

    [[nodiscard]] pid_t pid() const noexcept { return pid_; }

    [[nodiscard]] daemon::UniqueFd connect_client() {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            throw std::system_error(
                errno, std::generic_category(), "Failed to create client socket");
        }
        daemon::UniqueFd client(fd);
        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        if (socket_path_.size() >= sizeof(address.sun_path)) {
            throw std::runtime_error("Daemon socket path is too long");
        }
        std::memcpy(
            address.sun_path, socket_path_.c_str(), socket_path_.size() + 1);
        if (::connect(
                fd,
                reinterpret_cast<sockaddr*>(&address),
                sizeof(address))
            != 0) {
            throw std::system_error(
                errno, std::generic_category(), "Failed to connect to cha-daemon");
        }
        if (!daemon::configure_socket(client.get())) {
            throw std::runtime_error("Failed to configure client socket");
        }
        return client;
    }

    void send_signal(int signal) const {
        if (pid_ != -1) (void)::kill(pid_, signal);
    }

    int wait_for_exit(std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        int status = 0;
        while (std::chrono::steady_clock::now() < deadline) {
            drain_stderr();
            const pid_t result = ::waitpid(pid_, &status, WNOHANG);
            if (result == pid_) {
                pid_ = -1;
                drain_stderr();
                if (WIFEXITED(status)) return WEXITSTATUS(status);
                if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
                return -1;
            }
            if (result < 0 && errno != EINTR) {
                throw std::system_error(
                    errno, std::generic_category(), "Failed to wait for cha-daemon");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (pid_ != -1) {
            (void)::kill(pid_, SIGKILL);
            while (::waitpid(pid_, &status, 0) < 0 && errno == EINTR) {
            }
            pid_ = -1;
        }
        drain_stderr();
        throw std::runtime_error("cha-daemon did not exit before the wait timeout");
    }

    [[nodiscard]] const std::string& stderr_text() const { return stderr_text_; }

    [[nodiscard]] const std::string& socket_path() const { return socket_path_; }

private:
    static void set_cloexec(int fd) {
        const int flags = ::fcntl(fd, F_GETFD);
        if (flags < 0 || ::fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
            throw std::system_error(
                errno, std::generic_category(), "Failed to set FD_CLOEXEC");
        }
    }

    static void set_nonblock(int fd) {
        const int flags = ::fcntl(fd, F_GETFL);
        if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            throw std::system_error(
                errno, std::generic_category(), "Failed to set O_NONBLOCK");
        }
    }

    daemon::UniqueFd create_listen_socket() {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            throw std::system_error(
                errno, std::generic_category(), "Failed to create listen socket");
        }
        daemon::UniqueFd listen(fd);
        socket_path_ =
            (std::filesystem::temp_directory_path()
             / ("cha-daemon-" + std::to_string(::getpid()) + "-"
                + std::to_string(++serial_) + ".sock"))
                .string();
        ::unlink(socket_path_.c_str());
        sockaddr_un address {};
        address.sun_family = AF_UNIX;
        if (socket_path_.size() >= sizeof(address.sun_path)) {
            throw std::runtime_error("Daemon socket path is too long");
        }
        std::memcpy(
            address.sun_path, socket_path_.c_str(), socket_path_.size() + 1);
        if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address))
                != 0
            || ::listen(fd, 16) != 0) {
            throw std::system_error(
                errno, std::generic_category(), "Failed to listen on Unix socket");
        }
        return listen;
    }

    void drain_stderr() {
        char buffer[1024];
        while (true) {
            const ssize_t count = ::read(stderr_.get(), buffer, sizeof(buffer));
            if (count > 0) {
                stderr_text_.append(buffer, static_cast<std::size_t>(count));
                continue;
            }
            if (count == 0) return;
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return;
            return;
        }
    }

    inline static unsigned serial_{};
    pid_t pid_{-1};
    daemon::UniqueFd stderr_;
    std::string stderr_text_;
    std::string socket_path_;
};

} // namespace cha::test
