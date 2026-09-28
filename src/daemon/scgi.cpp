#include "daemon/scgi.h"

#include <cerrno>
#include <charconv>
#include <cstdint>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace cha::daemon {
namespace {

constexpr int poll_timeout_ms = 50;
constexpr std::size_t max_length_digits = 20;

enum class WaitStatus {
    ready,
    stopped,
    failed,
};

WaitStatus wait_socket(
    int fd, short events, const std::atomic<bool>& stop) {
    while (!stop.load(std::memory_order_relaxed)) {
        pollfd item{};
        item.fd = fd;
        item.events = events;
        const int result = ::poll(&item, 1, poll_timeout_ms);
        if (result < 0) {
            if (errno == EINTR) continue;
            return WaitStatus::failed;
        }
        if (result == 0) continue;
        if ((item.revents & (POLLERR | POLLNVAL)) != 0) {
            return WaitStatus::failed;
        }
        if ((item.revents & events) != 0) return WaitStatus::ready;
        if ((item.revents & POLLHUP) != 0) {
            if ((events & POLLIN) != 0) return WaitStatus::ready;
            return WaitStatus::failed;
        }
    }
    return WaitStatus::stopped;
}

bool would_block(int error) {
    return error == EAGAIN || error == EWOULDBLOCK;
}

enum class RecvStatus {
    data,
    eof,
    stopped,
    failed,
};

RecvStatus recv_bytes(
    int fd,
    char* destination,
    std::size_t size,
    std::size_t& received,
    const std::atomic<bool>& stop) {
    received = 0;
    while (received < size) {
        const ssize_t count = ::recv(
            fd, destination + received, size - received, 0);
        if (count > 0) {
            received += static_cast<std::size_t>(count);
            continue;
        }
        if (count == 0) return RecvStatus::eof;
        if (errno == EINTR) continue;
        if (would_block(errno)) {
            switch (wait_socket(fd, POLLIN, stop)) {
            case WaitStatus::ready:
                continue;
            case WaitStatus::stopped:
                return RecvStatus::stopped;
            case WaitStatus::failed:
                return RecvStatus::failed;
            }
        }
        return RecvStatus::failed;
    }
    return RecvStatus::data;
}

RecvStatus recv_one(
    int fd, char& byte, const std::atomic<bool>& stop) {
    std::size_t received = 0;
    return recv_bytes(fd, &byte, 1, received, stop);
}

ScgiReadResult incomplete_result() {
    return {ScgiReadStatus::incomplete, {}};
}

ScgiReadResult status_result(ScgiReadStatus status) {
    return {status, {}};
}

bool parse_size(std::string_view text, std::size_t& value) {
    if (text.empty()) return false;
    for (const char character : text) {
        if (character < '0' || character > '9') return false;
    }
    std::size_t parsed = 0;
    const auto result = std::from_chars(
        text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size()) {
        return false;
    }
    value = parsed;
    return true;
}

std::optional<std::string_view> next_token(
    std::string_view block, std::size_t& offset) {
    if (offset >= block.size()) return std::nullopt;
    const std::size_t end = block.find('\0', offset);
    if (end == std::string_view::npos) return std::nullopt;
    const std::string_view token = block.substr(offset, end - offset);
    offset = end + 1;
    return token;
}

bool parse_headers(
    std::string_view block,
    ScgiRequest& request,
    std::size_t& body_size,
    ScgiReadStatus& status) {
    std::size_t offset = 0;
    bool first = true;
    std::optional<std::string_view> content_length;
    std::optional<std::string_view> scgi;
    std::optional<std::string_view> method;
    std::optional<std::string_view> uri;
    while (offset < block.size()) {
        const auto name = next_token(block, offset);
        const auto value = next_token(block, offset);
        if (!name || !value) {
            status = ScgiReadStatus::bad_request;
            return false;
        }
        if (first) {
            if (*name != "CONTENT_LENGTH") {
                status = ScgiReadStatus::bad_request;
                return false;
            }
            first = false;
        }
        if (*name == "CONTENT_LENGTH") {
            if (content_length) {
                status = ScgiReadStatus::bad_request;
                return false;
            }
            content_length = *value;
        } else if (*name == "SCGI") {
            if (scgi) {
                status = ScgiReadStatus::bad_request;
                return false;
            }
            scgi = *value;
        } else if (*name == "REQUEST_METHOD") {
            if (method) {
                status = ScgiReadStatus::bad_request;
                return false;
            }
            method = *value;
        } else if (*name == "DOCUMENT_URI") {
            if (uri) {
                status = ScgiReadStatus::bad_request;
                return false;
            }
            uri = *value;
        }
    }
    if (!content_length || !scgi || !method || !uri) {
        status = ScgiReadStatus::bad_request;
        return false;
    }
    if (*scgi != "1" || method->empty() || uri->empty()) {
        status = ScgiReadStatus::bad_request;
        return false;
    }
    if (!parse_size(*content_length, body_size)) {
        status = ScgiReadStatus::bad_request;
        return false;
    }
    if (body_size > scgi_body_limit) {
        status = ScgiReadStatus::too_large;
        return false;
    }
    request.method = std::string(*method);
    request.document_uri = std::string(*uri);
    return true;
}

std::string_view reason_phrase(int status) {
    switch (status) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 413:
        return "Content Too Large";
    case 500:
        return "Internal Server Error";
    case 501:
        return "Not Implemented";
    case 502:
        return "Bad Gateway";
    default:
        return "OK";
    }
}

} // namespace

bool configure_socket(int fd) noexcept {
    const int descriptor_flags = ::fcntl(fd, F_GETFD);
    if (descriptor_flags < 0
        || ::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
        return false;
    }
    const int status_flags = ::fcntl(fd, F_GETFL);
    if (status_flags < 0
        || ::fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) < 0) {
        return false;
    }
#ifdef SO_NOSIGPIPE
    const int enable = 1;
    (void)::setsockopt(
        fd, SOL_SOCKET, SO_NOSIGPIPE, &enable, sizeof(enable));
#endif
    return true;
}

UniqueFd accept_connection(int listen_fd, const std::atomic<bool>& stop) {
    switch (wait_socket(listen_fd, POLLIN, stop)) {
    case WaitStatus::ready:
        break;
    case WaitStatus::stopped:
    case WaitStatus::failed:
        return {};
    }
    const int fd = ::accept(listen_fd, nullptr, nullptr);
    if (fd < 0) return {};
    UniqueFd client(fd);
    if (!configure_socket(client.get())) return {};
    return client;
}

ScgiReadResult read_scgi(int fd, const std::atomic<bool>& stop) {
    std::string digits;
    digits.reserve(max_length_digits);
    while (digits.size() < max_length_digits) {
        char byte = 0;
        switch (recv_one(fd, byte, stop)) {
        case RecvStatus::data:
            break;
        case RecvStatus::eof:
        case RecvStatus::stopped:
        case RecvStatus::failed:
            return incomplete_result();
        }
        if (byte == ':') break;
        if (byte < '0' || byte > '9') {
            return status_result(ScgiReadStatus::bad_request);
        }
        digits.push_back(byte);
    }
    if (digits.empty() || digits.size() == max_length_digits) {
        char byte = 0;
        if (digits.size() == max_length_digits) {
            switch (recv_one(fd, byte, stop)) {
            case RecvStatus::data:
                if (byte != ':') {
                    return status_result(ScgiReadStatus::bad_request);
                }
                break;
            default:
                return incomplete_result();
            }
        } else {
            return status_result(ScgiReadStatus::bad_request);
        }
    }

    std::size_t header_size = 0;
    if (!parse_size(digits, header_size)) {
        return status_result(ScgiReadStatus::bad_request);
    }
    if (header_size > scgi_header_limit) {
        return status_result(ScgiReadStatus::too_large);
    }

    std::string header(header_size + 1, '\0');
    std::size_t received = 0;
    switch (recv_bytes(
        fd, header.data(), header.size(), received, stop)) {
    case RecvStatus::data:
        break;
    case RecvStatus::eof:
    case RecvStatus::stopped:
    case RecvStatus::failed:
        return incomplete_result();
    }
    if (header.back() != ',') {
        return status_result(ScgiReadStatus::bad_request);
    }
    header.pop_back();

    ScgiReadResult result;
    std::size_t body_size = 0;
    if (!parse_headers(
            header, result.request, body_size, result.status)) {
        return result;
    }

    result.request.body.resize(body_size);
    if (body_size != 0) {
        std::size_t body_received = 0;
        switch (recv_bytes(
            fd,
            result.request.body.data(),
            body_size,
            body_received,
            stop)) {
        case RecvStatus::data:
            break;
        case RecvStatus::eof:
        case RecvStatus::stopped:
        case RecvStatus::failed:
            return incomplete_result();
        }
    }
    result.status = ScgiReadStatus::ok;
    return result;
}

bool peer_closed(int fd) {
    while (true) {
        char byte = 0;
        const ssize_t count = ::recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
        if (count == 0) return true;
        if (count > 0) return false;
        if (errno == EINTR) continue;
        if (would_block(errno)) return false;
        return true;
    }
}

bool write_bytes(
    int fd, std::string_view data, const std::atomic<bool>& stop) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t count = ::send(
            fd,
            data.data() + sent,
            data.size() - sent,
            MSG_NOSIGNAL);
        if (count > 0) {
            sent += static_cast<std::size_t>(count);
            continue;
        }
        if (count == 0) return false;
        if (errno == EINTR) continue;
        if (would_block(errno)) {
            switch (wait_socket(fd, POLLOUT, stop)) {
            case WaitStatus::ready:
                continue;
            case WaitStatus::stopped:
            case WaitStatus::failed:
                return false;
            }
        }
        return false;
    }
    return true;
}

bool write_cgi(
    int fd,
    int status,
    std::string_view content_type,
    std::string_view body,
    const std::atomic<bool>& stop) {
    std::string head = "Status: ";
    head += std::to_string(status);
    head += ' ';
    head += reason_phrase(status);
    head += "\r\nContent-Type: ";
    head += content_type;
    head += "\r\n\r\n";
    if (!write_bytes(fd, head, stop)) return false;
    return write_bytes(fd, body, stop);
}

} // namespace cha::daemon
