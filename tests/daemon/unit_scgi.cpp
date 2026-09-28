#include "daemon/scgi.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

namespace cha::daemon {
namespace {

struct SocketPair {
    UniqueFd local;
    UniqueFd peer;
};

SocketPair make_pair() {
    int fds[2]{};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        throw std::runtime_error("socketpair failed");
    }
    UniqueFd local(fds[0]);
    UniqueFd peer(fds[1]);
    if (!configure_socket(local.get()) || !configure_socket(peer.get())) {
        throw std::runtime_error("configure_socket failed");
    }
    return {std::move(local), std::move(peer)};
}

std::string scgi_block(
    const std::vector<std::pair<std::string, std::string>>& pairs,
    std::string_view body) {
    std::string headers;
    for (const auto& [name, value] : pairs) {
        headers.append(name);
        headers.push_back('\0');
        headers.append(value);
        headers.push_back('\0');
    }
    return std::to_string(headers.size()) + ":" + headers + ","
        + std::string(body);
}

std::string scgi_request(
    std::string_view method,
    std::string_view uri,
    std::string_view body,
    const std::vector<std::pair<std::string, std::string>>& extra = {}) {
    std::vector<std::pair<std::string, std::string>> pairs{
        {"CONTENT_LENGTH", std::to_string(body.size())},
        {"SCGI", "1"},
        {"REQUEST_METHOD", std::string(method)},
        {"DOCUMENT_URI", std::string(uri)},
    };
    pairs.insert(pairs.end(), extra.begin(), extra.end());
    return scgi_block(pairs, body);
}

void send_all(int fd, std::string_view data) {
    std::atomic<bool> stop{false};
    ASSERT_TRUE(write_bytes(fd, data, stop));
}

void send_fragmented(int fd, std::string_view data) {
    std::atomic<bool> stop{false};
    for (const char byte : data) {
        if (!write_bytes(fd, std::string_view(&byte, 1), stop)) return;
    }
}

TEST(Scgi, ReadsFragmentedHeadersAndBody) {
    auto sockets = make_pair();
    const std::string request = scgi_request(
        "POST", "/v1/chat/completions", "{\"model\":\"lobby\"}");
    std::thread writer([&] { send_fragmented(sockets.peer.get(), request); });
    std::atomic<bool> stop{false};
    const ScgiReadResult result = read_scgi(sockets.local.get(), stop);
    writer.join();
    ASSERT_EQ(result.status, ScgiReadStatus::ok);
    EXPECT_EQ(result.request.method, "POST");
    EXPECT_EQ(result.request.document_uri, "/v1/chat/completions");
    EXPECT_EQ(result.request.body, "{\"model\":\"lobby\"}");
    EXPECT_FALSE(peer_closed(sockets.local.get()));
}

TEST(Scgi, RejectsMalformedFraming) {
    const std::string valid = scgi_request("GET", "/v1/models", "");
    struct Case {
        std::string name;
        std::string payload;
    };
    const Case cases[]{
        {"missing colon digits", "GET /v1/models,"},
        {"empty length", ":" + valid.substr(valid.find(':') + 1)},
        {"broken pair", "7:CONTENT,"},
        {"content length not first",
         scgi_block(
             {{"SCGI", "1"},
              {"CONTENT_LENGTH", "0"},
              {"REQUEST_METHOD", "GET"},
              {"DOCUMENT_URI", "/v1/models"}},
             "")},
        {"duplicate method",
         scgi_block(
             {{"CONTENT_LENGTH", "0"},
              {"SCGI", "1"},
              {"REQUEST_METHOD", "GET"},
              {"DOCUMENT_URI", "/v1/models"},
              {"REQUEST_METHOD", "POST"}},
             "")},
        {"missing scgi",
         scgi_block(
             {{"CONTENT_LENGTH", "0"},
              {"REQUEST_METHOD", "GET"},
              {"DOCUMENT_URI", "/v1/models"}},
             "")},
        {"scgi not one",
         scgi_block(
             {{"CONTENT_LENGTH", "0"},
              {"SCGI", "0"},
              {"REQUEST_METHOD", "GET"},
              {"DOCUMENT_URI", "/v1/models"}},
             "")},
        {"non numeric content length",
         scgi_block(
             {{"CONTENT_LENGTH", "nope"},
              {"SCGI", "1"},
              {"REQUEST_METHOD", "GET"},
              {"DOCUMENT_URI", "/v1/models"}},
             "")},
        {"missing comma", "4:abcdx"},
        {"bytes after body",
         scgi_request("POST", "/v1/chat/completions", "{}") + "x"},
    };
    for (const auto& item : cases) {
        SCOPED_TRACE(item.name);
        auto sockets = make_pair();
        send_all(sockets.peer.get(), item.payload);
        sockets.peer.close();
        std::atomic<bool> stop{false};
        const ScgiReadResult result = read_scgi(sockets.local.get(), stop);
        EXPECT_EQ(result.status, ScgiReadStatus::bad_request);
    }
}

TEST(Scgi, RejectsIntegerOverflowInNetstringLength) {
    auto sockets = make_pair();
    send_all(sockets.peer.get(), "18446744073709551616:ignored,");
    sockets.peer.close();
    std::atomic<bool> stop{false};
    const ScgiReadResult result = read_scgi(sockets.local.get(), stop);
    EXPECT_EQ(result.status, ScgiReadStatus::too_large);
}

TEST(Scgi, RejectsHeaderAndBodySizeLimits) {
    {
        auto sockets = make_pair();
        send_all(
            sockets.peer.get(),
            std::to_string(scgi_header_limit + 1) + ":");
        std::atomic<bool> stop{false};
        const ScgiReadResult result = read_scgi(sockets.local.get(), stop);
        EXPECT_EQ(result.status, ScgiReadStatus::too_large);
    }
    {
        auto sockets = make_pair();
        send_all(
            sockets.peer.get(),
            scgi_block(
                {{"CONTENT_LENGTH", std::to_string(scgi_body_limit + 1)},
                 {"SCGI", "1"},
                 {"REQUEST_METHOD", "POST"},
                 {"DOCUMENT_URI", "/v1/chat/completions"}},
                ""));
        std::atomic<bool> stop{false};
        const ScgiReadResult result = read_scgi(sockets.local.get(), stop);
        EXPECT_EQ(result.status, ScgiReadStatus::too_large);
    }
}

TEST(Scgi, DropsPrematureEndOfFile) {
    auto sockets = make_pair();
    send_all(sockets.peer.get(), "12:CONTENT_LEN");
    sockets.peer.close();
    std::atomic<bool> stop{false};
    const ScgiReadResult result = read_scgi(sockets.local.get(), stop);
    EXPECT_EQ(result.status, ScgiReadStatus::incomplete);
}

TEST(Scgi, StopInterruptsABufferedRequest) {
    auto sockets = make_pair();
    send_all(
        sockets.peer.get(),
        scgi_request("POST", "/v1/chat/completions", "{}"));
    std::atomic<bool> stop{true};
    const ScgiReadResult result = read_scgi(sockets.local.get(), stop);
    EXPECT_EQ(result.status, ScgiReadStatus::incomplete);
}

TEST(Scgi, DetectsACompleteAbandonedRequest) {
    auto sockets = make_pair();
    const std::string request = scgi_request("GET", "/v1/models", "");
    send_all(sockets.peer.get(), request);
    ASSERT_EQ(::shutdown(sockets.peer.get(), SHUT_WR), 0);
    std::atomic<bool> stop{false};
    const ScgiReadResult result = read_scgi(sockets.local.get(), stop);
    ASSERT_EQ(result.status, ScgiReadStatus::ok);
    EXPECT_TRUE(peer_closed(sockets.local.get()));
}

TEST(Scgi, WritesCgiThroughPartialSends) {
    auto sockets = make_pair();
    const int send_buffer = 1024;
    ASSERT_EQ(::setsockopt(
        sockets.local.get(), SOL_SOCKET, SO_SNDBUF,
        &send_buffer, sizeof(send_buffer)), 0);
    const std::string body(200000, 'a');
    std::atomic<bool> stop{false};
    std::atomic<bool> finished{false};
    std::thread writer([&] {
        EXPECT_TRUE(write_cgi(
            sockets.local.get(), 200, "application/json", body, stop));
        finished = true;
        sockets.local.close();
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    EXPECT_FALSE(finished.load());
    std::string received;
    char buffer[64];
    while (true) {
        const ssize_t count =
            ::recv(sockets.peer.get(), buffer, sizeof(buffer), 0);
        if (count == 0) break;
        if (count < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            break;
        }
        received.append(buffer, static_cast<std::size_t>(count));
    }
    writer.join();
    EXPECT_NE(received.find("Status: 200 OK\r\n"), std::string::npos);
    EXPECT_TRUE(received.ends_with(body));
}

TEST(Scgi, ClosedOutputPeerDoesNotKillTheProcess) {
    auto sockets = make_pair();
    sockets.peer.close();
    std::atomic<bool> stop{false};
    const std::string body(65536, 'x');
    EXPECT_FALSE(write_cgi(
        sockets.local.get(), 200, "application/json", body, stop));
}

TEST(Scgi, IgnoresUnrelatedVariables) {
    auto sockets = make_pair();
    const std::string request = scgi_request(
        "GET",
        "/v1/models",
        "",
        {{"QUERY_STRING", "unused=1"}, {"SERVER_NAME", "nginx"}});
    send_all(sockets.peer.get(), request);
    std::atomic<bool> stop{false};
    const ScgiReadResult result = read_scgi(sockets.local.get(), stop);
    ASSERT_EQ(result.status, ScgiReadStatus::ok);
    EXPECT_EQ(result.request.method, "GET");
    EXPECT_EQ(result.request.document_uri, "/v1/models");
}

} // namespace
} // namespace cha::daemon
