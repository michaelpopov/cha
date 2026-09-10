#pragma once

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace cha {

// A loopback server that replays scripted HTTP responses and records the requests it received, so
// the provider transport can be tested over a real socket instead of a stubbed backend. It binds
// an ephemeral port reported by port(), serves one connection per scripted response on its own
// thread, and rethrows any failure from that thread in join().
class MockHttpServer {
#ifdef _WIN32
    using Socket = SOCKET;
    using PollDescriptor = WSAPOLLFD;
    static constexpr Socket invalid_socket = INVALID_SOCKET;
    static constexpr int socket_error = SOCKET_ERROR;
    static constexpr short read_event = POLLRDNORM;
    static constexpr int shutdown_both = SD_BOTH;
#else
    using Socket = int;
    using PollDescriptor = pollfd;
    static constexpr Socket invalid_socket = -1;
    static constexpr int socket_error = -1;
    static constexpr short read_event = POLLIN;
    static constexpr int shutdown_both = SHUT_RDWR;
#endif

public:
    explicit MockHttpServer(
        std::vector<std::string> responses,
        bool wait_for_client_close = false,
        std::chrono::milliseconds hold_response_open = {})
      : responses_(std::move(responses)),
        wait_for_client_close_(wait_for_client_close),
        hold_response_open_(hold_response_open) {
#ifdef _WIN32
        WSADATA sockets{};
        if (WSAStartup(MAKEWORD(2, 2), &sockets) != 0) {
            throw std::runtime_error("Failed to initialize mock server sockets");
        }
        sockets_initialized_ = true;
#endif
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == invalid_socket) {
            cleanup_sockets();
            throw std::runtime_error("Failed to create mock server socket");
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
                static_cast<int>(sizeof(address)))
                == socket_error
            || ::listen(listener_, 4) == socket_error) {
            close_socket(listener_);
            listener_ = invalid_socket;
            cleanup_sockets();
            throw std::runtime_error("Failed to bind mock server socket");
        }

#ifdef _WIN32
        int address_length = sizeof(address);
#else
        socklen_t address_length = sizeof(address);
#endif
        if (::getsockname(
                listener_,
                reinterpret_cast<sockaddr*>(&address),
                &address_length)
            == socket_error) {
            close_socket(listener_);
            listener_ = invalid_socket;
            cleanup_sockets();
            throw std::runtime_error("Failed to read mock server port");
        }
#if defined(__APPLE__) && defined(__MACH__)
        port_ = static_cast<int>(ntohs(address.sin_port));
#else
        port_ = static_cast<int>(::ntohs(address.sin_port));
#endif
    }

    ~MockHttpServer() {
        if (thread_.joinable()) {
            thread_.join();
        }
        if (listener_ != invalid_socket) {
            close_socket(listener_);
        }
        cleanup_sockets();
    }

    MockHttpServer(const MockHttpServer&) = delete;
    MockHttpServer& operator=(const MockHttpServer&) = delete;

    [[nodiscard]] int port() const {
        return port_;
    }

    void start() {
        thread_ = std::thread([this] {
            try {
                for (const std::string& response : responses_) {
                    const Socket client = accept_connection();
                    {
                        std::lock_guard lock(requests_mutex_);
                        requests_.push_back(read_request(client));
                    }
                    requests_changed_.notify_all();
                    send_all(client, response);
                    if (hold_response_open_.count() > 0) {
                        std::this_thread::sleep_for(hold_response_open_);
                    }
                    if (wait_for_client_close_) {
                        wait_for_client_close(client);
                    }
                    ::shutdown(client, shutdown_both);
                    close_socket(client);
                }
            } catch (...) {
                error_ = std::current_exception();
            }
        });
    }

    void join() {
        if (thread_.joinable()) {
            thread_.join();
        }
        if (error_) {
            std::rethrow_exception(error_);
        }
    }

    [[nodiscard]] const std::vector<std::string>& requests() const {
        return requests_;
    }

    [[nodiscard]] bool wait_for_requests(
        std::size_t count,
        std::chrono::milliseconds timeout) {
        std::unique_lock lock(requests_mutex_);
        return requests_changed_.wait_for(
            lock,
            timeout,
            [this, count] { return requests_.size() >= count; });
    }

private:
    static void close_socket(Socket socket) {
#ifdef _WIN32
        ::closesocket(socket);
#else
        ::close(socket);
#endif
    }

    void cleanup_sockets() {
#ifdef _WIN32
        if (sockets_initialized_) {
            WSACleanup();
            sockets_initialized_ = false;
        }
#endif
    }

    static int wait_for_socket(PollDescriptor& descriptor) {
#ifdef _WIN32
        return ::WSAPoll(&descriptor, 1, 5000);
#else
        return ::poll(&descriptor, 1, 5000);
#endif
    }

    [[nodiscard]] Socket accept_connection() const {
        PollDescriptor descriptor{listener_, read_event, 0};
        if (wait_for_socket(descriptor) != 1) {
            throw std::runtime_error("Timed out waiting for mock client");
        }

        const Socket client = ::accept(listener_, nullptr, nullptr);
        if (client == invalid_socket) {
            throw std::runtime_error("Failed to accept mock client");
        }
        return client;
    }

    [[nodiscard]] static std::string read_request(Socket client) {
        std::string request;
        std::array<char, 4096> buffer{};
        std::size_t expected_size = std::string::npos;

        while (expected_size == std::string::npos
            || request.size() < expected_size) {
            const auto bytes = ::recv(
                client,
                buffer.data(),
                static_cast<int>(buffer.size()),
                0);
            if (bytes <= 0) {
                throw std::runtime_error(
                    "Failed to read mock HTTP request");
            }
            request.append(
                buffer.data(),
                static_cast<std::size_t>(bytes));

            const std::size_t body_start = request.find("\r\n\r\n");
            if (body_start == std::string::npos) {
                continue;
            }

            const std::size_t length_start =
                request.find("Content-Length:");
            if (length_start == std::string::npos) {
                if (request.starts_with("GET ")) {
                    return request;
                }
                throw std::runtime_error(
                    "Mock request has no Content-Length header");
            }
            const std::size_t value_start =
                length_start
                + std::string_view("Content-Length:").size();
            const std::size_t value_end =
                request.find("\r\n", value_start);
            const std::size_t content_length = std::stoul(
                request.substr(value_start, value_end - value_start));
            expected_size = body_start + 4 + content_length;
        }

        return request;
    }

    static void send_all(Socket client, std::string_view response) {
        while (!response.empty()) {
            const int chunk = static_cast<int>(std::min<std::size_t>(
                response.size(),
                static_cast<std::size_t>(std::numeric_limits<int>::max())));
            const auto bytes = ::send(client, response.data(), chunk, 0);
            if (bytes <= 0) {
                throw std::runtime_error(
                    "Failed to send mock HTTP response");
            }
            response.remove_prefix(static_cast<std::size_t>(bytes));
        }
    }

    static void wait_for_client_close(Socket client) {
        PollDescriptor descriptor{
            client,
            static_cast<short>(read_event | POLLHUP | POLLERR),
            0,
        };
        if (wait_for_socket(descriptor) != 1) {
            throw std::runtime_error(
                "Timed out waiting for mock client to close");
        }

        char byte{};
        if (::recv(client, &byte, 1, 0) > 0) {
            throw std::runtime_error(
                "Mock client sent unexpected response data");
        }
    }

    Socket listener_{invalid_socket};
#ifdef _WIN32
    bool sockets_initialized_{};
#endif
    int port_{};
    std::vector<std::string> responses_;
    bool wait_for_client_close_{};
    std::chrono::milliseconds hold_response_open_{};
    std::mutex requests_mutex_;
    std::condition_variable requests_changed_;
    std::vector<std::string> requests_;
    std::exception_ptr error_;
    std::thread thread_;
};

inline std::string http_response(
    std::string_view content_type,
    const std::string& body) {
    return "HTTP/1.1 200 OK\r\nContent-Type: "
        + std::string(content_type)
        + "\r\nContent-Length: " + std::to_string(body.size())
        + "\r\nConnection: close\r\n\r\n" + body;
}

inline std::string request_body(const std::string& request) {
    const std::size_t body_start = request.find("\r\n\r\n");
    return request.substr(body_start + 4);
}

} // namespace cha
