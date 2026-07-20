#include "config.h"
#include "conversation.h"
#include "environment.h"
#include "pipe.h"
#include "server.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <string>

namespace cha {
namespace {

struct ChatResult {
    std::string response;
    std::size_t chunks{};
};

Config integration_config(bool stream) {
    const std::filesystem::path workspace_directory{CHA_WORKSPACE_DIRECTORY};
    load_dotenv(workspace_directory / ".env");
    Config config = Config::load(workspace_directory / "two" / "config.toml");
    config.stream = stream;
    return config;
}

ChatResult run_chat(bool stream) {
    const Config config = integration_config(stream);
    Pipe pipe_user2server;
    Pipe pipe_server2user;
    std::atomic_bool cancellation{false};
    Conversation conversation;
    Server server(cancellation, conversation);
    server.init(config);

    server.run(pipe_user2server, pipe_server2user);

    const std::string input = "Reply with one short sentence confirming that the connection works.";
    conversation.add_message(std::string(user_author), input);
    pipe_user2server.put(input);
    pipe_user2server.eom();

    ChatResult result;
    while (true) {
        const PipeEvent event = pipe_server2user.get();
        if (event.kind == PipeEventKind::eom) {
            break;
        }
        if (event.kind == PipeEventKind::conversation_updated) {
            ++result.chunks;
        }
    }

    const auto messages = conversation.messages();
    if (!messages.empty()) {
        result.response = messages.back().text;
    }

    server.stop();
    return result;
}

ChatResult run_cancelled_chat() {
    const Config config = integration_config(true);
    Pipe pipe_user2server;
    Pipe pipe_server2user;
    std::atomic_bool cancellation{false};
    Conversation conversation;
    Server server(cancellation, conversation);
    server.init(config);

    server.run(pipe_user2server, pipe_server2user);

    const std::string input = "Write a detailed essay of at least two thousand words about distributed systems.";
    conversation.add_message(std::string(user_author), input);
    pipe_user2server.put(input);
    pipe_user2server.eom();

    ChatResult result;
    while (true) {
        const PipeEvent event = pipe_server2user.get();
        if (event.kind == PipeEventKind::eom) {
            break;
        }
        if (event.kind == PipeEventKind::conversation_updated) {
            ++result.chunks;
            cancellation.store(true, std::memory_order_release);
        }
    }

    const auto messages = conversation.messages();
    if (!messages.empty()) {
        result.response = messages.back().text;
    }

    server.stop();
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
