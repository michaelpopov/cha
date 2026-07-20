#include "agent_protocol.h"
#include "config.h"
#include "environment.h"
#include "agent_worker.h"

#include <gtest/gtest.h>

#include <poll.h>

#include <cstddef>
#include <filesystem>
#include <stdexcept>
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

AgentEvent wait_for_agent_event(AgentWorker& worker) {
    pollfd descriptor{worker.notification_fd(), POLLIN, 0};
    if (::poll(&descriptor, 1, -1) != 1) {
        throw std::runtime_error(
            "Failed to wait for integration agent event");
    }
    AgentEvent event = AgentCompleted{};
    if (worker.try_receive(event) != ChannelReadStatus::value) {
        throw std::runtime_error(
            "Integration agent event channel closed unexpectedly");
    }
    return event;
}

ChatResult run_chat(bool stream) {
    const Config config = integration_config(stream);
    AgentWorker worker({.config = config});

    const std::string input = "Reply with one short sentence confirming that the connection works.";
    EXPECT_TRUE(worker.submit(
        {1, worker.info().id, {}, make_human_entry(1, input, 1)}));

    ChatResult result;
    while (true) {
        const AgentEvent event = wait_for_agent_event(worker);
        if (const auto* delta = std::get_if<AgentDelta>(&event)) {
            ++result.chunks;
            result.response += delta->text;
        } else {
            EXPECT_TRUE(std::holds_alternative<AgentCompleted>(event));
            break;
        }
    }

    worker.stop();
    return result;
}

ChatResult run_cancelled_chat() {
    const Config config = integration_config(true);
    AgentWorker worker({.config = config});

    const std::string input = "Write a detailed essay of at least two thousand words about distributed systems.";
    EXPECT_TRUE(worker.submit(
        {2, worker.info().id, {}, make_human_entry(1, input, 2)}));

    ChatResult result;
    while (true) {
        const AgentEvent event = wait_for_agent_event(worker);
        if (const auto* delta = std::get_if<AgentDelta>(&event)) {
            ++result.chunks;
            result.response += delta->text;
            worker.cancel();
        } else {
            EXPECT_TRUE(std::holds_alternative<AgentCancelled>(event));
            break;
        }
    }

    worker.stop();
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
