#include "support/xai_fake_server.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace cha {
namespace {

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket kInvalid = INVALID_SOCKET;
constexpr int kError = SOCKET_ERROR;
#else
using Socket = int;
constexpr Socket kInvalid = -1;
constexpr int kError = -1;
#endif

void close_socket(Socket socket) {
    if (socket == kInvalid) return;
#ifdef _WIN32
    ::closesocket(socket);
#else
    ::close(socket);
#endif
}

std::uint32_t rotate_left(std::uint32_t value, int bits) {
    return (value << bits) | (value >> (32 - bits));
}

std::array<unsigned char, 20> sha1(std::string_view input) {
    std::uint32_t h0 = 0x67452301U;
    std::uint32_t h1 = 0xEFCDAB89U;
    std::uint32_t h2 = 0x98BADCFEU;
    std::uint32_t h3 = 0x10325476U;
    std::uint32_t h4 = 0xC3D2E1F0U;
    std::string message(input);
    const std::uint64_t bits = static_cast<std::uint64_t>(message.size()) * 8U;
    message.push_back(static_cast<char>(0x80));
    while (message.size() % 64 != 56) message.push_back('\0');
    for (int shift = 56; shift >= 0; shift -= 8) {
        message.push_back(static_cast<char>((bits >> shift) & 0xffU));
    }
    for (std::size_t offset = 0; offset < message.size(); offset += 64) {
        std::uint32_t words[80] = {};
        for (int index = 0; index < 16; ++index) {
            const auto base = offset + static_cast<std::size_t>(index) * 4;
            words[index] = (static_cast<unsigned char>(message[base]) << 24)
                | (static_cast<unsigned char>(message[base + 1]) << 16)
                | (static_cast<unsigned char>(message[base + 2]) << 8)
                | static_cast<unsigned char>(message[base + 3]);
        }
        for (int index = 16; index < 80; ++index) {
            words[index] = rotate_left(
                words[index - 3] ^ words[index - 8] ^ words[index - 14]
                    ^ words[index - 16],
                1);
        }
        std::uint32_t a = h0;
        std::uint32_t b = h1;
        std::uint32_t c = h2;
        std::uint32_t d = h3;
        std::uint32_t e = h4;
        for (int index = 0; index < 80; ++index) {
            std::uint32_t f = 0;
            std::uint32_t k = 0;
            if (index < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999U;
            } else if (index < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1U;
            } else if (index < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDCU;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6U;
            }
            const std::uint32_t next = rotate_left(a, 5) + f + e + k + words[index];
            e = d;
            d = c;
            c = rotate_left(b, 30);
            b = a;
            a = next;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
    }
    std::array<unsigned char, 20> digest{};
    const std::uint32_t words[] = {h0, h1, h2, h3, h4};
    for (int index = 0; index < 5; ++index) {
        digest[static_cast<std::size_t>(index) * 4] =
            static_cast<unsigned char>((words[index] >> 24) & 0xffU);
        digest[static_cast<std::size_t>(index) * 4 + 1] =
            static_cast<unsigned char>((words[index] >> 16) & 0xffU);
        digest[static_cast<std::size_t>(index) * 4 + 2] =
            static_cast<unsigned char>((words[index] >> 8) & 0xffU);
        digest[static_cast<std::size_t>(index) * 4 + 3] =
            static_cast<unsigned char>(words[index] & 0xffU);
    }
    return digest;
}

std::string base64_encode(const unsigned char* data, std::size_t size) {
    static constexpr char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    for (std::size_t index = 0; index < size; index += 3) {
        const unsigned value = (static_cast<unsigned>(data[index]) << 16)
            | (index + 1 < size ? static_cast<unsigned>(data[index + 1]) << 8 : 0U)
            | (index + 2 < size ? static_cast<unsigned>(data[index + 2]) : 0U);
        encoded.push_back(table[(value >> 18) & 63U]);
        encoded.push_back(table[(value >> 12) & 63U]);
        encoded.push_back(index + 1 < size ? table[(value >> 6) & 63U] : '=');
        encoded.push_back(index + 2 < size ? table[value & 63U] : '=');
    }
    return encoded;
}

std::string header_value(std::string_view request, std::string_view name) {
    const std::string needle = "\r\n" + std::string(name) + ":";
    auto lower_request = std::string(request);
    auto lower_needle = needle;
    std::transform(lower_request.begin(), lower_request.end(), lower_request.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    std::transform(lower_needle.begin(), lower_needle.end(), lower_needle.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    const auto found = lower_request.find(lower_needle);
    if (found == std::string::npos) return {};
    const auto start = found + needle.size();
    auto end = request.find("\r\n", start);
    if (end == std::string::npos) end = request.size();
    std::string value(request.substr(start, end - start));
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
        value.erase(value.begin());
    }
    return value;
}

} // namespace

std::string xai_fake_websocket_accept(std::string_view key) {
    std::string material(key);
    material += "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    const auto digest = sha1(material);
    return base64_encode(digest.data(), digest.size());
}

struct XaiFakeServer::Impl {
    explicit Impl(XaiFakeServerOptions options) : options(std::move(options)) {
#ifdef _WIN32
        WSADATA sockets{};
        if (WSAStartup(MAKEWORD(2, 2), &sockets) != 0) {
            throw std::runtime_error("Failed to initialize the xAI fake server");
        }
        sockets_ready = true;
#endif
        listener = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener == kInvalid) fail("create");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(static_cast<std::uint16_t>(this->options.port));
        if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == kError
            || ::listen(listener, 1) == kError) {
            fail("bind");
        }
        socklen_t length = sizeof(address);
        if (::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == kError) {
            fail("port");
        }
        port = ntohs(address.sin_port);
    }

    ~Impl() {
        if (thread.joinable()) thread.join();
        close_socket(listener);
#ifdef _WIN32
        if (sockets_ready) WSACleanup();
#endif
    }

    void fail(const char* step) {
        close_socket(listener);
        listener = kInvalid;
#ifdef _WIN32
        if (sockets_ready) WSACleanup();
        sockets_ready = false;
#endif
        throw std::runtime_error(std::string("xAI fake server failed to ") + step);
    }

    void start() {
        thread = std::thread([this] {
            try {
                serve();
            } catch (...) {
                error = std::current_exception();
            }
        });
    }

    void join() {
        if (thread.joinable()) thread.join();
        if (error) std::rethrow_exception(error);
    }

    void serve() {
        const Socket client = accept_client();
        if (client == kInvalid) return;
        const std::string head = read_http(client);
        {
            std::lock_guard lock(mu);
            request = head;
        }
        if (options.http_status != 101) {
            const std::string response = "HTTP/1.1 " + std::to_string(options.http_status)
                + " Error\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
            send_all(client, response);
            close_socket(client);
            return;
        }
        const std::string key = header_value(head, "sec-websocket-key");
        if (key.empty()) throw std::runtime_error("xAI fake server saw no websocket key");
        const std::string response =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + xai_fake_websocket_accept(key) + "\r\n\r\n";
        send_all(client, response);
        if (options.send_ping) send_frame(client, 0x9, "ping", true);
        for (std::size_t index = 0; index < options.messages.size(); ++index) {
            const bool split = options.fragment_first && index == 0
                && options.messages[index].size() > 1;
            if (!split) {
                send_frame(client, 0x1, options.messages[index], true);
                continue;
            }
            const auto midpoint = options.messages[index].size() / 2;
            send_frame(client, 0x1, options.messages[index].substr(0, midpoint), false);
            send_frame(client, 0x0, options.messages[index].substr(midpoint), true);
        }
        if (options.close_after_messages) {
            send_frame(client, 0x8, "", true);
            close_socket(client);
            return;
        }
        const auto deadline = std::chrono::steady_clock::now() + options.read_timeout;
        std::string text;
        bool saw_done = false;
        while (std::chrono::steady_clock::now() < deadline && !saw_done) {
            const auto frame = read_frame(client, deadline);
            if (!frame) break;
            if (frame->opcode == 0x8) break;
            if (frame->opcode == 0x9) {
                send_frame(client, 0xA, frame->payload, true);
                continue;
            }
            if (frame->opcode == 0xA) {
                std::lock_guard lock(mu);
                pong = true;
                continue;
            }
            if (frame->opcode == 0x2 || (frame->opcode == 0x0 && binary_open)) {
                if (frame->opcode == 0x2) binary_partial = frame->payload;
                else binary_partial += frame->payload;
                binary_open = !frame->fin;
                if (frame->fin) {
                    remember_binary(binary_partial);
                    binary_partial.clear();
                }
                continue;
            }
            if (frame->opcode == 0x1 || frame->opcode == 0x0) {
                if (frame->opcode == 0x1) text.clear();
                text += frame->payload;
                if (frame->fin) {
                    std::lock_guard lock(mu);
                    texts.push_back(text);
                    events.push_back("t:" + text);
                    if (text == "{\"type\":\"audio.done\"}") saw_done = true;
                    text.clear();
                }
            }
        }
        if (saw_done) {
            for (const std::string& message : options.after_audio_done) {
                send_frame(client, 0x1, message, true);
            }
        }
        close_socket(client);
    }

    struct Frame {
        int opcode = 0;
        bool fin = true;
        std::string payload;
    };

    void remember_binary(const std::string& payload) {
        std::lock_guard lock(mu);
        binary.push_back(std::vector<unsigned char>(payload.begin(), payload.end()));
        events.push_back("b:" + std::to_string(payload.size()));
    }

    Socket accept_client() {
        const auto deadline = std::chrono::steady_clock::now() + options.read_timeout;
#ifdef _WIN32
        WSAPOLLFD descriptor{};
        descriptor.fd = listener;
        descriptor.events = POLLRDNORM;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (WSAPoll(&descriptor, 1, static_cast<int>(std::max<std::int64_t>(left.count(), 0))) <= 0) {
            return kInvalid;
        }
#else
        pollfd descriptor{};
        descriptor.fd = listener;
        descriptor.events = POLLIN;
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (poll(&descriptor, 1, static_cast<int>(std::max<std::int64_t>(left.count(), 0))) <= 0) {
            return kInvalid;
        }
#endif
        Socket client = ::accept(listener, nullptr, nullptr);
        if (client == kInvalid) return client;
        int one = 1;
        ::setsockopt(client, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<char*>(&one), sizeof(one));
#ifdef __APPLE__
        ::setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
        return client;
    }

    std::string read_http(Socket client) {
        std::string data;
        const auto deadline = std::chrono::steady_clock::now() + options.read_timeout;
        while (data.find("\r\n\r\n") == std::string::npos) {
            if (std::chrono::steady_clock::now() >= deadline || data.size() > 65536) {
                throw std::runtime_error("xAI fake server timed out reading the handshake");
            }
            char buffer[1024];
            const auto count = ::recv(client, buffer, sizeof(buffer), 0);
            if (count <= 0) throw std::runtime_error("xAI fake server lost the handshake");
            data.append(buffer, static_cast<std::size_t>(count));
        }
        return data;
    }

    std::optional<Frame> read_frame(
        Socket client,
        std::chrono::steady_clock::time_point deadline) {
        unsigned char header[2];
        if (!read_exact(client, header, 2, deadline)) return std::nullopt;
        Frame frame;
        frame.fin = (header[0] & 0x80) != 0;
        frame.opcode = header[0] & 0x0f;
        const bool masked = (header[1] & 0x80) != 0;
        std::uint64_t length = header[1] & 0x7f;
        if (length == 126) {
            unsigned char extended[2];
            if (!read_exact(client, extended, 2, deadline)) return std::nullopt;
            length = (static_cast<std::uint64_t>(extended[0]) << 8) | extended[1];
        } else if (length == 127) {
            unsigned char extended[8];
            if (!read_exact(client, extended, 8, deadline)) return std::nullopt;
            length = 0;
            for (unsigned char byte : extended) length = (length << 8) | byte;
        }
        if (length > 1024 * 1024) {
            throw std::runtime_error("xAI fake server rejected a large frame");
        }
        unsigned char mask[4] = {};
        if (masked && !read_exact(client, mask, 4, deadline)) return std::nullopt;
        frame.payload.resize(static_cast<std::size_t>(length));
        if (length > 0
            && !read_exact(
                client, reinterpret_cast<unsigned char*>(frame.payload.data()),
                frame.payload.size(), deadline)) {
            return std::nullopt;
        }
        if (masked) {
            for (std::size_t index = 0; index < frame.payload.size(); ++index) {
                frame.payload[index] = static_cast<char>(
                    static_cast<unsigned char>(frame.payload[index]) ^ mask[index % 4]);
            }
        }
        return frame;
    }

    bool read_exact(
        Socket client,
        unsigned char* data,
        std::size_t size,
        std::chrono::steady_clock::time_point deadline) {
        std::size_t got = 0;
        while (got < size) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            const auto count = ::recv(
                client, reinterpret_cast<char*>(data + got), size - got, 0);
            if (count <= 0) return false;
            got += static_cast<std::size_t>(count);
        }
        return true;
    }

    void send_all(Socket client, std::string_view bytes) {
        std::size_t sent = 0;
        while (sent < bytes.size()) {
            const std::size_t chunk = options.byte_writes ? 1 : bytes.size() - sent;
            const auto count = ::send(
                client, bytes.data() + sent, chunk, 0);
            if (count <= 0) throw std::runtime_error("xAI fake server send failed");
            sent += static_cast<std::size_t>(count);
        }
    }

    void send_frame(Socket client, int opcode, std::string_view payload, bool fin) {
        std::string frame;
        frame.push_back(static_cast<char>((fin ? 0x80 : 0) | opcode));
        if (payload.size() < 126) {
            frame.push_back(static_cast<char>(payload.size()));
        } else if (payload.size() <= 65535) {
            frame.push_back(126);
            frame.push_back(static_cast<char>((payload.size() >> 8) & 0xff));
            frame.push_back(static_cast<char>(payload.size() & 0xff));
        } else {
            frame.push_back(127);
            for (int shift = 56; shift >= 0; shift -= 8) {
                frame.push_back(static_cast<char>((payload.size() >> shift) & 0xff));
            }
        }
        frame.append(payload);
        send_all(client, frame);
    }

    XaiFakeServerOptions options;
    Socket listener = kInvalid;
    int port = 0;
    std::thread thread;
    std::exception_ptr error;
    std::mutex mu;
    std::string request;
    std::vector<std::vector<unsigned char>> binary;
    std::vector<std::string> texts;
    std::vector<std::string> events;
    std::string binary_partial;
    bool binary_open = false;
    bool pong = false;
#ifdef _WIN32
    bool sockets_ready = false;
#endif
};

XaiFakeServer::XaiFakeServer(XaiFakeServerOptions options)
    : impl_(new Impl(std::move(options))) {}

XaiFakeServer::~XaiFakeServer() { delete impl_; }

int XaiFakeServer::port() const { return impl_->port; }

void XaiFakeServer::start() { impl_->start(); }

void XaiFakeServer::join() { impl_->join(); }

std::string XaiFakeServer::request() const {
    std::lock_guard lock(impl_->mu);
    return impl_->request;
}

std::vector<std::vector<unsigned char>> XaiFakeServer::binary_messages() const {
    std::lock_guard lock(impl_->mu);
    return impl_->binary;
}

std::vector<std::string> XaiFakeServer::text_messages() const {
    std::lock_guard lock(impl_->mu);
    return impl_->texts;
}

std::vector<std::string> XaiFakeServer::events() const {
    std::lock_guard lock(impl_->mu);
    return impl_->events;
}

bool XaiFakeServer::saw_pong() const {
    std::lock_guard lock(impl_->mu);
    return impl_->pong;
}

int run_xai_fixture_server(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: cha_xai_fake_server <fixture.jsonl> [--port N]\n";
        return 2;
    }
    int port = 0;
    for (int index = 2; index < argc; ++index) {
        if (std::string_view(argv[index]) == "--port" && index + 1 < argc) {
            port = std::stoi(argv[++index]);
        } else {
            std::cerr << "usage: cha_xai_fake_server <fixture.jsonl> [--port N]\n";
            return 2;
        }
    }
    std::ifstream input(argv[1]);
    if (!input) {
        std::cerr << "xAI fake server could not read the fixture\n";
        return 1;
    }
    XaiFakeServerOptions options;
    options.port = port;
    options.read_timeout = std::chrono::milliseconds(60000);
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const auto row = nlohmann::json::parse(line);
        const auto event = row.at("event");
        const std::string type = event.at("type").get<std::string>();
        if (type == "transcript.done") options.after_audio_done.push_back(event.dump());
        else options.messages.push_back(event.dump());
    }
    XaiFakeServer server(std::move(options));
    std::cout << "ws://127.0.0.1:" << server.port() << std::endl;
    server.start();
    server.join();
    return 0;
}

} // namespace cha
