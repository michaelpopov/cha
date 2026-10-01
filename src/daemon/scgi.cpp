#include "daemon/scgi.h"

#include <cerrno>
#include <charconv>
#include <optional>
#include <system_error>

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

bool would_block(int error) {
    return error == EAGAIN || error == EWOULDBLOCK;
}

// Waits until the socket is ready for `events`. The short poll lets the
// caller see `stop`. Returns false on a socket error or when `stop` is set.
bool wait_socket(int fd, short events, const std::atomic<bool>& stop) {
    while (!stop.load()) {
        pollfd item{};
        item.fd = fd;
        item.events = events;
        const int result = ::poll(&item, 1, poll_timeout_ms);
        if (result < 0 && errno != EINTR) return false;
        if (result <= 0) continue;
        if ((item.revents & events) != 0) return true;
        // After a hang-up, recv() still reads the end of the stream.
        return (item.revents & POLLHUP) != 0 && (events & POLLIN) != 0;
    }
    return false;
}

// Reads exactly `size` bytes. Returns false at the end of the stream, on a
// socket error or when `stop` is set.
bool recv_all(
    int fd,
    char* destination,
    std::size_t size,
    const std::atomic<bool>& stop) {
    std::size_t received = 0;
    while (received < size) {
        if (stop.load()) return false;
        const ssize_t count =
            ::recv(fd, destination + received, size - received, 0);
        if (count > 0) {
            received += static_cast<std::size_t>(count);
        } else if (count == 0) {
            return false;
        } else if (errno != EINTR
            && (!would_block(errno) || !wait_socket(fd, POLLIN, stop))) {
            return false;
        }
    }
    return true;
}

bool parse_size(std::string_view text, std::size_t& value) {
    const char* const end = text.data() + text.size();
    const auto [stop, error] = std::from_chars(text.data(), end, value);
    return !text.empty() && error == std::errc() && stop == end;
}

std::optional<std::string_view> next_token(
    std::string_view block, std::size_t& offset) {
    const std::size_t end = block.find('\0', offset);
    if (end == std::string_view::npos) return std::nullopt;
    const std::string_view token = block.substr(offset, end - offset);
    offset = end + 1;
    return token;
}

// Reads the NUL-separated name/value pairs. CONTENT_LENGTH must come first.
// Variables that the daemon does not use are ignored.
ScgiReadStatus parse_headers(
    std::string_view block, ScgiRequest& request, std::size_t& body_size) {
    std::optional<std::string_view> content_length;
    std::optional<std::string_view> scgi;
    std::optional<std::string_view> method;
    std::optional<std::string_view> uri;
    std::optional<std::string_view> content_type;
    std::optional<std::string_view> audio_offset;
    std::size_t offset = 0;
    while (offset < block.size()) {
        const auto name = next_token(block, offset);
        const auto value = next_token(block, offset);
        if (!name || !value) return ScgiReadStatus::bad_request;
        if (!content_length && *name != "CONTENT_LENGTH") {
            return ScgiReadStatus::bad_request;
        }
        std::optional<std::string_view>* field = nullptr;
        if (*name == "CONTENT_LENGTH") field = &content_length;
        else if (*name == "SCGI") field = &scgi;
        else if (*name == "REQUEST_METHOD") field = &method;
        else if (*name == "DOCUMENT_URI") field = &uri;
        else if (*name == "CONTENT_TYPE") field = &content_type;
        else if (*name == "HTTP_X_CHA_AUDIO_OFFSET") field = &audio_offset;
        if (field == nullptr) continue;
        if (*field) return ScgiReadStatus::bad_request;
        *field = *value;
    }
    if (!content_length || !method || !uri || scgi != "1"
        || method->empty() || uri->empty()
        || !parse_size(*content_length, body_size)) {
        return ScgiReadStatus::bad_request;
    }
    if (body_size > scgi_body_limit) return ScgiReadStatus::too_large;
    request.method = std::string(*method);
    request.document_uri = std::string(*uri);
    if (content_type) request.content_type = std::string(*content_type);
    if (audio_offset) request.audio_offset = std::string(*audio_offset);
    return ScgiReadStatus::ok;
}

std::string_view reason_phrase(int status) {
    switch (status) {
    case 201:
        return "Created";
    case 204:
        return "No Content";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 409:
        return "Conflict";
    case 413:
        return "Content Too Large";
    case 415:
        return "Unsupported Media Type";
    case 422:
        return "Unprocessable Entity";
    case 500:
        return "Internal Server Error";
    case 502:
        return "Bad Gateway";
    case 503:
        return "Service Unavailable";
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
    if (!wait_socket(listen_fd, POLLIN, stop)) return {};
    UniqueFd client(::accept(listen_fd, nullptr, nullptr));
    if (!client || !configure_socket(client.get())) return {};
    return client;
}

ScgiReadResult read_scgi(int fd, const std::atomic<bool>& stop) {
    // The netstring length ends at ':'. The check against the limit after
    // each digit also prevents overflow.
    std::size_t header_size = 0;
    for (std::size_t digits = 0;; ++digits) {
        char byte = 0;
        if (!recv_all(fd, &byte, 1, stop)) return {ScgiReadStatus::incomplete};
        if (byte == ':' && digits > 0) break;
        if (byte < '0' || byte > '9') return {ScgiReadStatus::bad_request};
        header_size = header_size * 10 + static_cast<std::size_t>(byte - '0');
        if (header_size > scgi_header_limit) return {ScgiReadStatus::too_large};
    }

    std::string header(header_size + 1, '\0');  // with the ',' after it
    if (!recv_all(fd, header.data(), header.size(), stop)) {
        return {ScgiReadStatus::incomplete};
    }
    if (header.back() != ',') return {ScgiReadStatus::bad_request};
    header.pop_back();

    ScgiReadResult result;
    std::size_t body_size = 0;
    result.status = parse_headers(header, result.request, body_size);
    if (result.status != ScgiReadStatus::ok) return {result.status};
    result.request.body.resize(body_size);
    if (!recv_all(fd, result.request.body.data(), body_size, stop)) {
        return {ScgiReadStatus::incomplete};
    }

    // SCGI uses one request per connection. Bytes beyond CONTENT_LENGTH are
    // not another request; accepting them would also make peer_closed() see a
    // permanently readable socket and miss an abandoned request.
    while (true) {
        char extra = 0;
        const ssize_t count =
            ::recv(fd, &extra, 1, MSG_PEEK | MSG_DONTWAIT);
        if (count > 0) return {ScgiReadStatus::bad_request};
        if (count == 0) break;
        if (errno == EINTR) continue;
        if (would_block(errno)) break;
        return {ScgiReadStatus::incomplete};
    }
    return result;
}

bool peer_closed(int fd) {
    while (true) {
        char byte = 0;
        const ssize_t count = ::recv(fd, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
        if (count == 0) return true;
        // No input is valid after the one SCGI request body. Treat trailing
        // bytes like a disconnect so they cannot mask peer closure forever.
        if (count > 0) return true;
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
            fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (count > 0) {
            sent += static_cast<std::size_t>(count);
        } else if (count == 0) {
            return false;
        } else if (errno != EINTR
            && (!would_block(errno) || !wait_socket(fd, POLLOUT, stop))) {
            return false;
        }
    }
    return true;
}

bool write_cgi(
    int fd,
    int status,
    std::string_view content_type,
    std::string_view body,
    const std::atomic<bool>& stop,
    std::string_view extra_headers) {
    std::string head = "Status: ";
    head += std::to_string(status);
    head += ' ';
    head += reason_phrase(status);
    head += "\r\n";
    if (status != 204) {
        head += "Content-Type: ";
        head += content_type;
        head += "\r\n";
    }
    head += extra_headers;
    head += "\r\n";
    if (status == 204) return write_bytes(fd, head, stop);
    return write_bytes(fd, head, stop) && write_bytes(fd, body, stop);
}

} // namespace cha::daemon
