#include "agent_protocol.h"
#include "config.h"
#include "environment.h"
#include "agent.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <variant>

namespace cha {
namespace {

// Captures the response text and streaming chunk count produced by an integration chat run.
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
    CompletionRequestChannel requests;
    AgentEventChannel events;
    std::atomic_bool cancellation{false};
    Agent agent(cancellation);
    agent.init(config);

    agent.run(requests, events);

    const std::string input = "Reply with one short sentence confirming that the connection works.";
    requests.push({1, agent.info().id, {}, input});

    ChatResult result;
    while (true) {
        const std::optional<AgentEvent> event = events.get();
        if (!event) {
            break;
        }
        if (const auto* delta = std::get_if<AgentDelta>(&*event)) {
            ++result.chunks;
            result.response += delta->text;
        } else {
            EXPECT_TRUE(std::holds_alternative<AgentCompleted>(*event));
            break;
        }
    }

    agent.stop();
    return result;
}

ChatResult run_cancelled_chat() {
    const Config config = integration_config(true);
    CompletionRequestChannel requests;
    AgentEventChannel events;
    std::atomic_bool cancellation{false};
    Agent agent(cancellation);
    agent.init(config);

    agent.run(requests, events);

    const std::string input = "Write a detailed essay of at least two thousand words about distributed systems.";
    requests.push({2, agent.info().id, {}, input});

    ChatResult result;
    while (true) {
        const std::optional<AgentEvent> event = events.get();
        if (!event) {
            break;
        }
        if (const auto* delta = std::get_if<AgentDelta>(&*event)) {
            ++result.chunks;
            result.response += delta->text;
            cancellation.store(true, std::memory_order_release);
        } else {
            EXPECT_TRUE(std::holds_alternative<AgentCancelled>(*event));
            break;
        }
    }

    agent.stop();
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
