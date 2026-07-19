#include "config.h"
#include "pipe.h"
#include "server.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>

namespace cha {
namespace {

struct ChatResult {
    std::string response;
    std::size_t chunks{};
};

Config integration_config(bool stream) {
    Config config;
    config.host = "alien";
    config.port = 8080;
    config.mode = Mode::net;
    config.model = "Qwen3.6-35B-A3B-UD-Q4_K_M.gguf";
    config.stream = stream;
    return config;
}

ChatResult run_chat(bool stream) {
    const Config config = integration_config(stream);
    Pipe pipe_user2server;
    Pipe pipe_server2user;
    std::atomic_bool cancellation{false};
    Server server(config, cancellation);

    server.run(pipe_user2server, pipe_server2user);

    pipe_user2server.put("Reply with one short sentence confirming that the connection works.");
    pipe_user2server.eom();

    ChatResult result;
    while (true) {
        const PipeEvent event = pipe_server2user.get();
        if (event.kind != PipeEventKind::data) {
            break;
        }
        result.response += event.data;
        ++result.chunks;
    }

    pipe_user2server.close();
    server.close();
    return result;
}

ChatResult run_cancelled_chat() {
    const Config config = integration_config(true);
    Pipe pipe_user2server;
    Pipe pipe_server2user;
    std::atomic_bool cancellation{false};
    Server server(config, cancellation);

    server.run(pipe_user2server, pipe_server2user);

    pipe_user2server.put("Write a detailed essay of at least two thousand words about distributed systems.");
    pipe_user2server.eom();

    ChatResult result;
    const PipeEvent first_chunk = pipe_server2user.get();
    if (first_chunk.kind == PipeEventKind::data) {
        result.response += first_chunk.data;
        ++result.chunks;
        cancellation.store(true, std::memory_order_release);
        while (true) {
            const PipeEvent event = pipe_server2user.get();
            if (event.kind != PipeEventKind::data) {
                break;
            }
            result.response += event.data;
            ++result.chunks;
        }
    }

    pipe_user2server.close();
    server.close();
    return result;
}

void expect_successful_chat(const ChatResult& result) {
    EXPECT_GT(result.chunks, 0U);
    EXPECT_FALSE(result.response.empty());
    EXPECT_FALSE(result.response.starts_with("Error:")) << result.response;
}

TEST(Integration, StreamingChatCompletesFullCycle) {
    expect_successful_chat(run_chat(true));
}

TEST(Integration, NonStreamingChatCompletesFullCycle) {
    expect_successful_chat(run_chat(false));
}

TEST(Integration, StreamingChatCanBeCancelled) {
    const ChatResult result = run_cancelled_chat();
    EXPECT_GT(result.chunks, 0U);
    EXPECT_FALSE(result.response.starts_with("Error:")) << result.response;
}

} // namespace
} // namespace cha
