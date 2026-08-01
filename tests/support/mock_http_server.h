#pragma once

#include <arpa/inet.h>
#include <array>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace cha {

// A loopback server that replays scripted HTTP responses and records the requests it received, so
// the completion transport can be tested over a real socket instead of a stubbed backend. It binds
// an ephemeral port reported by port(), serves one connection per scripted response on its own
// thread, and rethrows any failure from that thread in join().
class MockHttpServer {
public:
    explicit MockHttpServer(
        std::vector<std::string> responses,
        bool wait_for_client_close = false)
      : responses_(std::move(responses)),
        wait_for_client_close_(wait_for_client_close) {
        listener_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == -1) {
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
                sizeof(address))
                == -1
            || ::listen(listener_, 4) == -1) {
            ::close(listener_);
            throw std::runtime_error("Failed to bind mock server socket");
        }

        socklen_t address_length = sizeof(address);
        if (::getsockname(
                listener_,
                reinterpret_cast<sockaddr*>(&address),
                &address_length)
            == -1) {
            ::close(listener_);
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
        if (listener_ != -1) {
            ::close(listener_);
        }
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
                    const int client = accept_connection();
                    {
                        std::lock_guard lock(requests_mutex_);
                        requests_.push_back(read_request(client));
                    }
                    requests_changed_.notify_all();
                    send_all(client, response);
                    if (wait_for_client_close_) {
                        wait_for_client_close(client);
                    }
                    ::shutdown(client, SHUT_RDWR);
                    ::close(client);
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
    [[nodiscard]] int accept_connection() const {
        pollfd descriptor{listener_, POLLIN, 0};
        if (::poll(&descriptor, 1, 5000) != 1) {
            throw std::runtime_error("Timed out waiting for mock client");
        }

        const int client = ::accept(listener_, nullptr, nullptr);
        if (client == -1) {
            throw std::runtime_error("Failed to accept mock client");
        }
        return client;
    }

    [[nodiscard]] static std::string read_request(int client) {
        std::string request;
        std::array<char, 4096> buffer{};
        std::size_t expected_size = std::string::npos;

        while (expected_size == std::string::npos
            || request.size() < expected_size) {
            const ssize_t bytes =
                ::recv(client, buffer.data(), buffer.size(), 0);
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

    static void send_all(int client, std::string_view response) {
        while (!response.empty()) {
            const ssize_t bytes =
                ::send(client, response.data(), response.size(), 0);
            if (bytes <= 0) {
                throw std::runtime_error(
                    "Failed to send mock HTTP response");
            }
            response.remove_prefix(static_cast<std::size_t>(bytes));
        }
    }

    static void wait_for_client_close(int client) {
        pollfd descriptor{
            client,
            POLLIN | POLLHUP | POLLERR,
            0,
        };
        if (::poll(&descriptor, 1, 5000) != 1) {
            throw std::runtime_error(
                "Timed out waiting for mock client to close");
        }

        char byte{};
        if (::recv(client, &byte, 1, 0) > 0) {
            throw std::runtime_error(
                "Mock client sent unexpected response data");
        }
    }

    int listener_{-1};
    int port_{};
    std::vector<std::string> responses_;
    bool wait_for_client_close_{};
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
