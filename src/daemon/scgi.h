#pragma once

#include <atomic>
#include <cstddef>
#include <string>
#include <string_view>
#include <unistd.h>

namespace cha::daemon {

inline constexpr std::size_t scgi_header_limit = 64 * 1024;
inline constexpr std::size_t scgi_body_limit = 16 * 1024 * 1024;

class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd() { close(); }

    UniqueFd(UniqueFd&& other) noexcept : fd_(other.fd_) { other.fd_ = -1; }
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            close();
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }

    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] explicit operator bool() const noexcept { return fd_ >= 0; }
    int release() noexcept {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }
    void close() noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_{-1};
};

struct ScgiRequest {
    std::string method;
    std::string document_uri;
    std::string body;
    std::string content_type;
    std::string audio_offset;
};

enum class ScgiReadStatus {
    ok,
    incomplete,
    bad_request,
    too_large,
};

struct ScgiReadResult {
    ScgiReadStatus status{ScgiReadStatus::incomplete};
    ScgiRequest request;
};

bool configure_socket(int fd) noexcept;
UniqueFd accept_connection(int listen_fd, const std::atomic<bool>& stop);
ScgiReadResult read_scgi(int fd, const std::atomic<bool>& stop);
bool peer_closed(int fd);
bool write_bytes(
    int fd, std::string_view data, const std::atomic<bool>& stop);
bool write_cgi(
    int fd,
    int status,
    std::string_view content_type,
    std::string_view body,
    const std::atomic<bool>& stop,
    std::string_view extra_headers = {});

} // namespace cha::daemon
