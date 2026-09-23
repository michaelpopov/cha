#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <vector>

namespace cha {

struct XaiFakeServerOptions {
    // Text frames sent after the handshake, before the server waits for audio.done.
    std::vector<std::string> messages;
    // Text frames sent after a client text frame {"type":"audio.done"}.
    std::vector<std::string> after_audio_done;
    bool fragment_first = false;
    bool byte_writes = false;
    bool send_ping = false;
    bool split_ping = false;
    bool close_after_messages = false;
    int http_status = 101;
    int port = 0;
    std::chrono::milliseconds read_timeout{3000};
};

// Loopback ws:// server for one connection. It is test-only.
class XaiFakeServer {
public:
    explicit XaiFakeServer(XaiFakeServerOptions options);
    ~XaiFakeServer();
    XaiFakeServer(const XaiFakeServer&) = delete;
    XaiFakeServer& operator=(const XaiFakeServer&) = delete;

    [[nodiscard]] int port() const;
    void start();
    void join();
    [[nodiscard]] std::string request() const;
    [[nodiscard]] std::vector<std::vector<unsigned char>> binary_messages() const;
    [[nodiscard]] std::vector<std::string> text_messages() const;
    [[nodiscard]] std::vector<std::string> events() const;
    [[nodiscard]] bool saw_pong() const;
    [[nodiscard]] std::vector<std::string> pong_messages() const;

private:
    struct Impl;
    Impl* impl_;
};

[[nodiscard]] std::string xai_fake_websocket_accept(std::string_view key);
[[nodiscard]] std::string xai_fake_base64(const std::vector<unsigned char>& bytes);

// Prints ws://127.0.0.1:<port>, replays one fixture to one connection, then
// prints the client messages it received. Returns 0 on success.
int run_xai_fixture_server(int argc, char** argv);

} // namespace cha
