#include "agents/agent_definition.h"
#include "agents/agent_protocol.h"
#include "agents/agent_registry.h"
#include "application/chat_coordinator.h"
#include "agents/config.h"
#include "interfaces/text/text_input.h"
#include "storage/agent_definition_loader.h"
#include "storage/config_loader.h"
#include "util/environment.h"
#include "support/mock_http_server.h"
#include "storage/session_database.h"
#include "storage/workspace.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <poll.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace cha {
namespace {

using Json = nlohmann::json;

// Captures the response text and streaming chunk count produced by an integration chat run.
struct ChatResult {
    std::string response;
    std::size_t chunks{};
};

Config integration_config(bool stream) {
    const std::filesystem::path workspace_directory{CHA_WORKSPACE_DIRECTORY};
    load_dotenv(workspace_directory / ".env");
    Config config = load_config(
        workspace_directory / "personas" / "Ismael" / "config.toml");
    config.stream = stream;
    return config;
}

AgentEvent wait_for_agent_event(AgentRegistry& registry) {
    pollfd descriptor{registry.notification_fd(), POLLIN, 0};
    if (::poll(&descriptor, 1, -1) != 1) {
        throw std::runtime_error(
            "Failed to wait for integration agent event");
    }
    AgentEvent event = AgentCompleted{};
    if (registry.try_receive(event) != ChannelReadStatus::value) {
        throw std::runtime_error(
            "Integration agent event channel closed unexpectedly");
    }
    return event;
}

ChatResult run_chat(bool stream) {
    const Config config = integration_config(stream);
    Conversation conversation;
    std::vector<AgentDefinition> definitions;
    definitions.push_back({.config = config});
    AgentRegistry registry(conversation, std::move(definitions));

    const std::string input = "Reply with one short sentence confirming that the connection works.";
    CompletionRequest request{
        .request_id = 1,
        .prompt = make_human_entry(1, config.id, config.name, input, 1),
    };
    conversation.add_entry(request.prompt);
    EXPECT_TRUE(registry.submit(std::move(request)));

    ChatResult result;
    while (true) {
        const AgentEvent event = wait_for_agent_event(registry);
        if (const auto* delta = std::get_if<AgentDelta>(&event)) {
            ++result.chunks;
            result.response += delta->text;
        } else {
            EXPECT_TRUE(std::holds_alternative<AgentCompleted>(event));
            break;
        }
    }

    registry.stop();
    return result;
}

ChatResult run_cancelled_chat() {
    const Config config = integration_config(true);
    Conversation conversation;
    std::vector<AgentDefinition> definitions;
    definitions.push_back({.config = config});
    AgentRegistry registry(conversation, std::move(definitions));

    const std::string input = "Write a detailed essay of at least two thousand words about distributed systems.";
    CompletionRequest request{
        .request_id = 2,
        .prompt = make_human_entry(1, config.id, config.name, input, 2),
    };
    conversation.add_entry(request.prompt);
    EXPECT_TRUE(registry.submit(std::move(request)));

    ChatResult result;
    while (true) {
        const AgentEvent event = wait_for_agent_event(registry);
        if (const auto* delta = std::get_if<AgentDelta>(&event)) {
            ++result.chunks;
            result.response += delta->text;
            registry.cancel();
        } else {
            EXPECT_TRUE(std::holds_alternative<AgentCancelled>(event));
            break;
        }
    }

    registry.stop();
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

// Removes one temporary session database when a multi-agent test leaves scope.
class TemporarySession {
public:
    TemporarySession()
      : path(std::filesystem::temp_directory_path()
             / ("cha_multi_agent_"
                + std::to_string(
                    std::chrono::steady_clock::now().time_since_epoch().count())
                + ".sqlite3")) {
        if (!create_session_database(
                path,
                {.id = "multi-agent", .room = "lobby", .label = "Multi-agent"})) {
            throw std::runtime_error("Failed to create the integration session database");
        }
    }

    ~TemporarySession() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    std::filesystem::path path;
};

// Loads the checked-in two-persona lobby room exactly as main() does.
std::vector<AgentDefinition> lobby_definitions() {
    const Workspace workspace{std::filesystem::path{CHA_WORKSPACE_DIRECTORY}};
    const Room room = workspace.load_room("lobby");
    std::vector<std::filesystem::path> directories;
    for (const std::string& persona_name : room.persona_names) {
        directories.push_back(workspace.persona_directory(persona_name));
    }
    return load_agent_definitions(directories, room.directory);
}

// Redirects one agent's backend at a local mock server without touching its prompt.
void point_at(AgentDefinition& definition, int port) {
    definition.config.host = "127.0.0.1";
    definition.config.port = port;
    definition.config.https = false;
    definition.config.mode = Mode::net;
    definition.config.stream = false;
    definition.config.api_key = "integration-key";
    definition.config.api_key_env.clear();
}

std::string answer(std::string_view text) {
    return http_response(
        "application/json",
        Json{{"choices", Json::array({Json{{"message", Json{{"content", text}}}}})}}
            .dump());
}

void run_until_idle(ChatCoordinator& coordinator) {
    while (coordinator.generating()) {
        pollfd descriptor{coordinator.notification_fd(), POLLIN, 0};
        if (::poll(&descriptor, 1, 5000) != 1) {
            throw std::runtime_error("Timed out waiting for an integration turn");
        }
        (void)coordinator.receive();
    }
}

Json body_of(const MockHttpServer& server) {
    return Json::parse(request_body(server.requests().front()));
}

std::string streamed_answer(
    std::string_view reasoning,
    std::string_view answer_text) {
    const std::string body =
        "data: "
        + Json{{"choices",
                Json::array({
                    Json{{"delta",
                          Json{{"reasoning_content", reasoning}}}},
                })}}
              .dump()
        + "\n\n"
        + "data: "
        + Json{{"choices",
                Json::array({
                    Json{{"delta", Json{{"content", answer_text}}}},
                })}}
              .dump()
        + "\n\n"
        + "data: [DONE]\n\n";
    return http_response("text/event-stream", body);
}

TEST(ReasoningIntegration, StreamsLiveReasoningButPersistsAndReplaysOnlyAnswer) {
    std::vector<AgentDefinition> definitions = lobby_definitions();
    definitions.resize(1);
    constexpr std::string_view reasoning_marker =
        "INTEGRATION_PRIVATE_REASONING_731";
    MockHttpServer server({
        streamed_answer(reasoning_marker, "First answer"),
        streamed_answer("SECOND_PRIVATE_REASONING", "Second answer"),
    });
    server.start();
    point_at(definitions.front(), server.port());
    definitions.front().config.stream = true;
    definitions.front().config.reasoning_format =
        ReasoningFormat::automatic;

    TemporarySession session;
    {
        ChatCoordinator coordinator(std::move(definitions), session.path);
        (void)handle_text_input(coordinator, "First question");
        run_until_idle(coordinator);
        const std::vector<ConversationEntry> live =
            coordinator.conversation().entries();
        ASSERT_EQ(live.size(), 2U);
        EXPECT_EQ(live.back().reasoning_text, reasoning_marker);
        EXPECT_EQ(live.back().text, "First answer");

        (void)handle_text_input(coordinator, "Second question");
        run_until_idle(coordinator);
    }
    server.join();

    ASSERT_EQ(server.requests().size(), 2U);
    const Json second_body =
        Json::parse(request_body(server.requests().back()));
    const std::string serialized = second_body.dump();
    EXPECT_EQ(serialized.find(reasoning_marker), std::string::npos);
    EXPECT_NE(serialized.find("First answer"), std::string::npos);

    const std::vector<ConversationEntry> restored =
        load_conversation_entries(session.path);
    ASSERT_EQ(restored.size(), 4U);
    for (const ConversationEntry& entry : restored) {
        EXPECT_TRUE(entry.reasoning_text.empty());
    }
    EXPECT_EQ(restored[1].text, "First answer");
    EXPECT_EQ(restored[3].text, "Second answer");
}

TEST(ReasoningIntegration, ParsesNonStreamingStructuredReasoning) {
    std::vector<AgentDefinition> definitions = lobby_definitions();
    definitions.resize(1);
    MockHttpServer server({http_response(
        "application/json",
        R"({"choices":[{"message":{"reasoning":"Non-stream thought","content":"Non-stream answer"}}]})")});
    server.start();
    point_at(definitions.front(), server.port());
    definitions.front().config.reasoning_format =
        ReasoningFormat::reasoning;

    TemporarySession session;
    ChatCoordinator coordinator(std::move(definitions), session.path);
    (void)handle_text_input(coordinator, "Question");
    run_until_idle(coordinator);
    server.join();

    const std::vector<ConversationEntry> live =
        coordinator.conversation().entries();
    ASSERT_EQ(live.size(), 2U);
    EXPECT_EQ(live.back().reasoning_text, "Non-stream thought");
    EXPECT_EQ(live.back().text, "Non-stream answer");
    const std::vector<ConversationEntry> restored =
        load_conversation_entries(session.path);
    EXPECT_TRUE(restored.back().reasoning_text.empty());
    EXPECT_EQ(restored.back().text, "Non-stream answer");
}

TEST(MultiAgentIntegration, RoutesEachPromptToItsOwnAgentOverItsOwnTransport) {
    std::vector<AgentDefinition> definitions = lobby_definitions();
    ASSERT_EQ(definitions.size(), 2U);
    ASSERT_EQ(definitions.front().config.name, "Cheburashka");
    ASSERT_EQ(definitions.back().config.name, "Ismael");
    const std::string cheburashka_prompt = definitions.front().system_prompt;
    const std::string ismael_prompt = definitions.back().system_prompt;
    ASSERT_NE(cheburashka_prompt, ismael_prompt);

    MockHttpServer cheburashka_server({answer("I am Cheburashka.")});
    MockHttpServer ismael_server({answer("Call me Ismael.")});
    cheburashka_server.start();
    ismael_server.start();
    point_at(definitions.front(), cheburashka_server.port());
    point_at(definitions.back(), ismael_server.port());

    TemporarySession session;
    {
        ChatCoordinator coordinator(std::move(definitions), session.path);
        ASSERT_EQ(coordinator.roster().first().id, "cheburashka");
        EXPECT_TRUE(coordinator.show_addressing());

        // No mention: the first persona in personas.list answers.
        CoordinatorUpdate update = handle_text_input(coordinator, "Who are you?");
        ASSERT_TRUE(update.clear_input);
        run_until_idle(coordinator);

        // An addressed prompt reaches the mentioned agent instead.
        update = handle_text_input(coordinator, "@Ismael, and you?");
        ASSERT_TRUE(update.clear_input);
        run_until_idle(coordinator);
    }
    cheburashka_server.join();
    ismael_server.join();

    ASSERT_EQ(cheburashka_server.requests().size(), 1U)
        << "the mentioned turn must not reach the default agent";
    ASSERT_EQ(ismael_server.requests().size(), 1U);

    const Json first = body_of(cheburashka_server);
    EXPECT_EQ(first["messages"], Json::array({
        Json{{"role", "system"}, {"content", cheburashka_prompt}},
        Json{{"role", "user"}, {"content", "Who are you?"}},
    }));

    // Ismael's own system prompt, and Cheburashka's answer attributed as user input.
    const Json second = body_of(ismael_server);
    EXPECT_EQ(second["messages"], Json::array({
        Json{{"role", "system"}, {"content", ismael_prompt}},
        Json{{"role", "user"},
             {"content",
              "User: [to Cheburashka] Who are you?"
              "\n\nCheburashka: I am Cheburashka."
              "\n\nUser: and you?"}},
    }));

    const std::vector<ConversationEntry> restored =
        load_conversation_entries(session.path);
    ASSERT_EQ(restored.size(), 4U);
    EXPECT_EQ(restored[0].addressed_to, "cheburashka");
    EXPECT_EQ(restored[0].text, "Who are you?");
    EXPECT_EQ(restored[1].participant_id, "cheburashka");
    EXPECT_EQ(restored[1].text, "I am Cheburashka.");
    EXPECT_EQ(restored[2].addressed_to, "ismael");
    EXPECT_EQ(restored[2].addressed_to_name, "Ismael");
    EXPECT_EQ(restored[2].text, "and you?")
        << "the mention is stripped from the stored prompt";
    EXPECT_EQ(restored[3].participant_id, "ismael");
    EXPECT_EQ(restored[3].display_name, "Ismael");
}

TEST(MultiAgentIntegration, ReopensTheSessionWhenTheRoomKeepsOnlyOneAgent) {
    std::vector<AgentDefinition> definitions = lobby_definitions();
    const std::string ismael_prompt = definitions.back().system_prompt;

    MockHttpServer cheburashka_server({answer("I am Cheburashka.")});
    MockHttpServer ismael_server({answer("Call me Ismael."), answer("He greeted you.")});
    cheburashka_server.start();
    ismael_server.start();
    point_at(definitions.front(), cheburashka_server.port());
    point_at(definitions.back(), ismael_server.port());
    AgentDefinition ismael_only = definitions.back();

    TemporarySession session;
    {
        ChatCoordinator coordinator(std::move(definitions), session.path);
        (void)handle_text_input(coordinator, "Who are you?");
        run_until_idle(coordinator);
        (void)handle_text_input(coordinator, "@Ismael and you?");
        run_until_idle(coordinator);
    }
    cheburashka_server.join();

    // Cheburashka has left personas.list; the stored session still opens.
    ConversationRestore restored = load_conversation_state(session.path);
    ASSERT_EQ(restored.entries.size(), 4U);
    ASSERT_TRUE(restored.interrupted_turns.empty());

    ChatCoordinator reopened(
        std::vector<AgentDefinition>{std::move(ismael_only)},
        session.path,
        std::move(restored));
    EXPECT_EQ(reopened.roster().agents().size(), 1U);
    EXPECT_TRUE(reopened.show_addressing())
        << "history involving a departed agent keeps addressing visible";
    EXPECT_EQ(
        handle_text_input(reopened, "@Cheburashka are you there?").notice,
        "Unknown agent @Cheburashka. Agents in this room: @Ismael");

    (void)handle_text_input(reopened, "What did he say?");
    run_until_idle(reopened);
    ismael_server.join();

    ASSERT_EQ(ismael_server.requests().size(), 2U);
    const Json body = Json::parse(request_body(ismael_server.requests().back()));
    EXPECT_EQ(body["messages"], Json::array({
        Json{{"role", "system"}, {"content", ismael_prompt}},
        Json{{"role", "user"},
             {"content",
              "User: [to Cheburashka] Who are you?"
              "\n\nCheburashka: I am Cheburashka."
              "\n\nUser: and you?"}},
        Json{{"role", "assistant"}, {"content", "Call me Ismael."}},
        Json{{"role", "user"}, {"content", "User: What did he say?"}},
    }));
}

} // namespace
} // namespace cha
