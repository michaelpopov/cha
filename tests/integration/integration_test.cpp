#include "agents/agent.h"
#include "agents/agent_registry.h"
#include "session/session_controller.h"
#include "agents/config.h"
#include "util/environment.h"
#include "util/thread_pool.h"
#include "support/mock_http_server.h"
#include "session/session_database.h"
#include "application/workspace_model.h"
#include "support/test_notifier.h"
#include "support/test_controller.h"
#include "support/test_session_database.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
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

test::TestNotifier& notifier() {
    static test::TestNotifier instance;
    return instance;
}

std::vector<TranscriptEntry> copy_entries(const Transcript& transcript) {
    const auto entries = transcript.entries();
    return {entries.begin(), entries.end()};
}

// Captures the response text and streaming chunk count produced by an integration chat run.
struct ChatResult {
    std::string response;
    std::size_t chunks{};
};

using IntegrationClock = std::chrono::steady_clock;
using IntegrationDeadline = IntegrationClock::time_point;

// Live providers may take several seconds before yielding their first stream
// event. Keep the test finite, while allowing normal queueing and startup
// latency. The deadline applies to the complete chat, not each individual
// chunk.
constexpr auto integration_chat_timeout = std::chrono::seconds(60);

class AgentRunCleanup final {
public:
    AgentRunCleanup(AgentRegistry& registry, ThreadPool& pool)
        : registry_(registry), pool_(pool) {
    }

    ~AgentRunCleanup() {
        // curl's progress callback observes this cancellation while a live
        // request is blocked, allowing clear_batch() and pool shutdown to join
        // the worker safely even when the test exits through an exception.
        registry_.cancel_batch();
        registry_.clear_batch();
        registry_.stop();
        pool_.stop();
    }

    AgentRunCleanup(const AgentRunCleanup&) = delete;
    AgentRunCleanup& operator=(const AgentRunCleanup&) = delete;

private:
    AgentRegistry& registry_;
    ThreadPool& pool_;
};

Config integration_config(bool stream) {
    const std::filesystem::path workspace_directory{CHA_WORKSPACE_DIRECTORY};
    load_dotenv(workspace_directory / ".env");
    Config config = load_config({
        .application_provider = load_provider_config(workspace_directory / "workspace.toml"),
        .definition = workspace_directory / "characters" / "Ismael" / "character.toml",
        .forum_defaults = workspace_directory / "forums" / "lobby" / "members" / "character_defaults.toml",
    }).config;
    config.stream = stream;
    return config;
}

AgentEvent wait_for_agent_event(
    AgentRegistry& registry,
    IntegrationDeadline deadline) {
    while (true) {
        const std::size_t observed = notifier().wake_count();
        AgentEvent event = AgentCompleted{};
        const ChannelReadStatus status = registry.try_receive(0, event);
        if (status == ChannelReadStatus::value) {
            return event;
        }
        if (status == ChannelReadStatus::closed) {
            throw std::runtime_error(
                "Integration agent event queue closed unexpectedly");
        }

        const auto now = IntegrationClock::now();
        if (now >= deadline) {
            registry.cancel_batch();
            throw std::runtime_error(
                "Timed out after 60 seconds waiting for an integration agent "
                "event; cancelled the live request");
        }
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - now);
        if (remaining.count() == 0) {
            remaining = std::chrono::milliseconds(1);
        }
        if (!notifier().wait_for_wake(
                observed,
                remaining)) {
            registry.cancel_batch();
            throw std::runtime_error(
                "Timed out after 60 seconds waiting for an integration agent "
                "event; cancelled the live request");
        }
    }
}

void start_agent_run(AgentRegistry& registry, CompletionInput input) {
    registry.stage_batch(std::vector<CompletionInput>{std::move(input)});
    registry.open_gate();
}

ChatResult run_chat(bool stream) {
    const Config config = integration_config(stream);
    Transcript transcript;
    std::vector<AgentDefinition> definitions;
    definitions.push_back({.config = config});
    ThreadPool pool(1);
    AgentRegistry registry(
        std::move(definitions),
        notifier(),
        pool);
    const AgentRunCleanup cleanup(registry, pool);

    const std::string input = "Reply with one short sentence confirming that the connection works.";
    CompletionInput request{
        .history = std::make_shared<const CompletionHistory>(
            transcript.completion_history()),
        .run = {
            .request_id = 1,
            .target = {config.id, config.display_name},
            .prompt_text = input,
        },
    };
    start_agent_run(registry, std::move(request));

    ChatResult result;
    const IntegrationDeadline deadline =
        IntegrationClock::now() + integration_chat_timeout;
    while (true) {
        const AgentEvent event = wait_for_agent_event(registry, deadline);
        if (const auto* delta = std::get_if<AgentDelta>(&event)) {
            ++result.chunks;
            result.response += delta->text;
        } else {
            EXPECT_TRUE(std::holds_alternative<AgentCompleted>(event));
            break;
        }
    }

    return result;
}

ChatResult run_cancelled_chat() {
    const Config config = integration_config(true);
    Transcript transcript;
    std::vector<AgentDefinition> definitions;
    definitions.push_back({.config = config});
    ThreadPool pool(1);
    AgentRegistry registry(
        std::move(definitions),
        notifier(),
        pool);
    const AgentRunCleanup cleanup(registry, pool);

    const std::string input = "Write a detailed essay of at least two thousand words about distributed systems.";
    CompletionInput request{
        .history = std::make_shared<const CompletionHistory>(
            transcript.completion_history()),
        .run = {
            .request_id = 2,
            .target = {config.id, config.display_name},
            .prompt_text = input,
        },
    };
    start_agent_run(registry, std::move(request));

    ChatResult result;
    const IntegrationDeadline deadline =
        IntegrationClock::now() + integration_chat_timeout;
    while (true) {
        const AgentEvent event = wait_for_agent_event(registry, deadline);
        if (const auto* delta = std::get_if<AgentDelta>(&event)) {
            ++result.chunks;
            result.response += delta->text;
            registry.cancel_batch();
        } else {
            EXPECT_TRUE(std::holds_alternative<AgentCancelled>(event));
            break;
        }
    }

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
                {.id = "multi-agent", .forum = "lobby", .label = "Multi-agent"})) {
            throw std::runtime_error("Failed to create the integration session database");
        }
    }

    ~TemporarySession() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    std::filesystem::path path;
};

// Loads the checked-in two-character lobby forum exactly as main() does.
struct LobbySetup {
    std::vector<AgentDefinition> definitions;
    PersonaRoster personas;
};

LobbySetup lobby_setup() {
    const std::filesystem::path root{CHA_WORKSPACE_DIRECTORY};
    const WorkspaceConfig config = load_workspace_config(root);
    const WorkspaceModel model = WorkspaceModel::load(root, config);
    const ForumInfo* const forum = model.find_forum("lobby");
    if (forum == nullptr) throw std::runtime_error("Checked-in workspace has no lobby forum");
    const std::filesystem::path forum_directory = root / "forums" / "lobby";
    std::vector<AgentDefinitionSource> sources;
    for (const std::string& character_id : forum->member_ids) {
        sources.push_back({
            .definition_directory = root / "characters" / character_id,
            .member_directory = forum_directory / "members" / character_id,
        });
    }
    // The mock provider needs mutable definitions, so this loads its own copy
    // from explicit fixture paths rather than reaching into the model's
    // private, provider-bearing values.
    PersonaRoster personas = *model.personas();
    return {
        .definitions = load_agent_definitions(
            sources,
            forum_directory,
            forum->display_name,
            personas,
            forum_directory / "members" / "character_defaults.toml",
            config.provider),
        .personas = std::move(personas),
    };
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

void run_until_idle(SessionController& controller) {
    while (controller.is_generating()) {
        const std::size_t observed = notifier().wake_count();
        (void)test::receive_all_events(controller);
        if (controller.is_generating()
            && !notifier().wait_for_wake(
                observed,
                std::chrono::seconds(5))) {
            throw std::runtime_error("Timed out waiting for an integration turn");
        }
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

TEST(ReasoningIntegration, ExcludesStreamedReasoningFromTranscriptAndModelContext) {
    LobbySetup lobby = lobby_setup();
    std::vector<AgentDefinition>& definitions = lobby.definitions;
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
        auto controller = test::from_definitions_for_testing(
            std::move(definitions),
            lobby.personas,
            session.path,
            notifier());
        (void)controller->submit_prompt("reader", "First question");
        run_until_idle(*controller);
        const std::vector<TranscriptEntry> live =
            copy_entries(controller->transcript());
        ASSERT_EQ(live.size(), 2U);
        EXPECT_EQ(live.back().text, "First answer");

        (void)controller->submit_prompt("reader", "Second question");
        run_until_idle(*controller);
    }
    server.join();

    ASSERT_EQ(server.requests().size(), 2U);
    const Json second_body =
        Json::parse(request_body(server.requests().back()));
    const std::string serialized = second_body.dump();
    EXPECT_EQ(serialized.find(reasoning_marker), std::string::npos);
    EXPECT_NE(serialized.find("First answer"), std::string::npos);

    const std::vector<TranscriptEntry> restored =
        load_transcript_entries(session.path);
    ASSERT_EQ(restored.size(), 4U);
    EXPECT_EQ(restored[1].text, "First answer");
    EXPECT_EQ(restored[3].text, "Second answer");
}

TEST(ReasoningIntegration, ExcludesNonStreamingReasoningFromTranscript) {
    LobbySetup lobby = lobby_setup();
    std::vector<AgentDefinition>& definitions = lobby.definitions;
    definitions.resize(1);
    MockHttpServer server({http_response(
        "application/json",
        R"({"choices":[{"message":{"reasoning":"Non-stream thought","content":"Non-stream answer"}}]})")});
    server.start();
    point_at(definitions.front(), server.port());
    definitions.front().config.reasoning_format =
        ReasoningFormat::reasoning;

    TemporarySession session;
    auto controller = test::from_definitions_for_testing(
        std::move(definitions),
        lobby.personas,
        session.path,
        notifier());
    (void)controller->submit_prompt("reader", "Question");
    run_until_idle(*controller);
    server.join();

    const std::vector<TranscriptEntry> live =
        copy_entries(controller->transcript());
    ASSERT_EQ(live.size(), 2U);
    EXPECT_EQ(live.back().text, "Non-stream answer");
    const std::vector<TranscriptEntry> restored =
        load_transcript_entries(session.path);
    EXPECT_EQ(restored.back().text, "Non-stream answer");
}

TEST(OffrecordIntegration, OmitsHiddenTurnsFromTheSerializedNextRequest) {
    LobbySetup lobby = lobby_setup();
    std::vector<AgentDefinition>& definitions = lobby.definitions;
    definitions.resize(1);
    const std::string system_prompt = definitions.front().system_prompt;
    MockHttpServer server({
        answer("Visible answer"),
        answer("Hidden answer"),
        answer("Current answer"),
    });
    server.start();
    point_at(definitions.front(), server.port());

    TemporarySession session;
    {
        auto controller = test::from_definitions_for_testing(
            std::move(definitions),
            lobby.personas,
            session.path,
            notifier());
        (void)controller->submit_prompt("reader", "Visible question");
        run_until_idle(*controller);
        EXPECT_TRUE(controller->open_offrecord().state_changed);
        (void)controller->submit_prompt("reader", "Hidden question");
        run_until_idle(*controller);
        EXPECT_TRUE(controller->extend_offrecord().state_changed);
        (void)controller->submit_prompt("reader", "Current question");
        run_until_idle(*controller);
    }
    server.join();

    ASSERT_EQ(server.requests().size(), 3U);
    const Json current_body =
        Json::parse(request_body(server.requests().back()));
    EXPECT_EQ(current_body["messages"], Json::array({
        Json{{"role", "system"}, {"content", system_prompt}},
        Json{{"role", "user"}, {"content", "from Reader:\nVisible question"}},
        Json{{"role", "assistant"}, {"content", "Visible answer"}},
        Json{{"role", "user"}, {"content", "from Reader:\nCurrent question"}},
    }));

    const std::vector<TranscriptEntry> restored =
        load_transcript_entries(session.path);
    ASSERT_EQ(restored.size(), 6U);
    EXPECT_EQ(restored[2].text, "Hidden question");
    EXPECT_EQ(restored[3].text, "Hidden answer");
}

TEST(MultiAgentIntegration, RoutesEachPromptToItsOwnAgentOverItsOwnTransport) {
    LobbySetup lobby = lobby_setup();
    std::vector<AgentDefinition>& definitions = lobby.definitions;
    ASSERT_EQ(definitions.size(), 2U);
    ASSERT_EQ(definitions.front().config.display_name, "Cheburashka");
    ASSERT_EQ(definitions.back().config.display_name, "Ismael");
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
        auto controller = test::from_definitions_for_testing(
            std::move(definitions),
            lobby.personas,
            session.path,
            notifier());
        ASSERT_EQ(controller->characters().first().id, "Cheburashka");

        // No mention: the first character directory in name order answers.
        SessionChange update =
            controller->submit_prompt("reader", "Who are you?");
        ASSERT_TRUE(update.input_consumed);
        run_until_idle(*controller);

        // An addressed prompt reaches the mentioned agent instead.
        update = controller->submit_prompt("reader", "and you?", "Ismael");
        ASSERT_TRUE(update.input_consumed);
        run_until_idle(*controller);
    }
    cheburashka_server.join();
    ismael_server.join();

    ASSERT_EQ(cheburashka_server.requests().size(), 1U)
        << "the mentioned turn must not reach the default agent";
    ASSERT_EQ(ismael_server.requests().size(), 1U);

    const Json first = body_of(cheburashka_server);
    EXPECT_EQ(first["messages"], Json::array({
        Json{{"role", "system"}, {"content", cheburashka_prompt}},
        Json{{"role", "user"}, {"content", "from Reader:\nWho are you?"}},
    }));

    // Ismael's own system prompt, and Cheburashka's answer attributed as persona input.
    const Json second = body_of(ismael_server);
    EXPECT_EQ(second["messages"], Json::array({
        Json{{"role", "system"}, {"content", ismael_prompt}},
        Json{{"role", "user"},
             {"content",
              "Shared chat history (JSONL):\n"
              R"({"kind":"human","speaker":"Reader","addressed_to":"Cheburashka","text":"Who are you?"})"
              "\n"
              R"({"kind":"agent","speaker":"Cheburashka","text":"I am Cheburashka."})"}},
        Json{{"role", "user"}, {"content", "from Reader:\nand you?"}},
    }));

    const std::vector<TranscriptEntry> restored =
        load_transcript_entries(session.path);
    ASSERT_EQ(restored.size(), 4U);
    EXPECT_EQ(restored[0].addressed_to, "Cheburashka");
    EXPECT_EQ(restored[0].text, "Who are you?");
    EXPECT_EQ(restored[1].participant_id, "Cheburashka");
    EXPECT_EQ(restored[1].text, "I am Cheburashka.");
    EXPECT_EQ(restored[2].addressed_to, "Ismael");
    EXPECT_EQ(restored[2].addressed_to_name, "Ismael");
    EXPECT_EQ(restored[2].text, "and you?")
        << "the mention is stripped from the stored prompt";
    EXPECT_EQ(restored[3].participant_id, "Ismael");
    EXPECT_EQ(restored[3].display_name, "Ismael");
}

TEST(MultiAgentIntegration, MulticastSendsIndependentBodiesAndRestoresHistory) {
    LobbySetup lobby = lobby_setup();
    std::vector<AgentDefinition>& definitions = lobby.definitions;
    ASSERT_EQ(definitions.size(), 2U);
    const std::string cheburashka_prompt = definitions.front().system_prompt;
    const std::string ismael_prompt = definitions.back().system_prompt;
    MockHttpServer cheburashka_server({
        answer("Cheburashka multicast answer"),
        answer("Cheburashka follow-up answer"),
    });
    MockHttpServer ismael_server({answer("Ismael multicast answer")});
    cheburashka_server.start();
    ismael_server.start();
    point_at(definitions.front(), cheburashka_server.port());
    point_at(definitions.back(), ismael_server.port());

    TemporarySession session;
    {
        auto controller = test::from_definitions_for_testing(
            std::move(definitions), lobby.personas, session.path,
            notifier());
        const SessionChange multicast = controller->start_multicast(
            "reader", "What time is it?", {});
        ASSERT_TRUE(multicast.input_consumed);
        run_until_idle(*controller);

        (void)controller->submit_prompt("reader", "What did the panel say?");
        run_until_idle(*controller);
    }
    cheburashka_server.join();
    ismael_server.join();

    ASSERT_EQ(cheburashka_server.requests().size(), 2U);
    ASSERT_EQ(ismael_server.requests().size(), 1U);
    EXPECT_EQ(
        Json::parse(request_body(cheburashka_server.requests()[0]))["messages"],
        Json::array({
            Json{{"role", "system"}, {"content", cheburashka_prompt}},
        Json{{"role", "user"}, {"content", "from Reader:\nWhat time is it?"}},
        }));
    EXPECT_EQ(
        body_of(ismael_server)["messages"],
        Json::array({
            Json{{"role", "system"}, {"content", ismael_prompt}},
        Json{{"role", "user"}, {"content", "from Reader:\nWhat time is it?"}},
        }));

    const std::string follow_up =
        Json::parse(request_body(cheburashka_server.requests()[1])).dump();
    EXPECT_NE(follow_up.find("Cheburashka multicast answer"), std::string::npos);
    EXPECT_NE(follow_up.find("Ismael multicast answer"), std::string::npos);
    const std::vector<TranscriptEntry> restored =
        load_transcript_entries(session.path);
    ASSERT_EQ(restored.size(), 6U);
    EXPECT_EQ(restored[0].text, "What time is it?");
    EXPECT_EQ(restored[2].text, "What time is it?");
    EXPECT_EQ(restored[4].text, "What did the panel say?");
}

TEST(MultiAgentIntegration, ReopensTheSessionWhenTheForumKeepsOnlyOneAgent) {
    LobbySetup lobby = lobby_setup();
    std::vector<AgentDefinition>& definitions = lobby.definitions;
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
        auto controller = test::from_definitions_for_testing(
            std::move(definitions),
            lobby.personas,
            session.path,
            notifier());
        (void)controller->submit_prompt("reader", "Who are you?");
        run_until_idle(*controller);
        (void)controller->submit_prompt("reader", "and you?", "Ismael");
        run_until_idle(*controller);
    }
    cheburashka_server.join();

    // Cheburashka has left the forum; the stored session still opens.
    SessionRestore restored = load_session_state(session.path);
    ASSERT_EQ(restored.entries.size(), 4U);
    ASSERT_TRUE(restored.interrupted_turns.empty());

    auto reopened = test::from_definitions_for_testing(
        std::vector<AgentDefinition>{std::move(ismael_only)},
        lobby.personas,
        session.path,
        notifier(),
        std::move(restored));
    EXPECT_EQ(reopened->characters().all().size(), 1U);
    EXPECT_EQ(
        reopened->submit_prompt(
            "reader", "are you there?", "Cheburashka").notice,
        "Unknown agent @Cheburashka. Characters in this forum: @Ismael");

    (void)reopened->submit_prompt("reader", "What did he say?");
    run_until_idle(*reopened);
    ismael_server.join();

    ASSERT_EQ(ismael_server.requests().size(), 2U);
    const Json body = Json::parse(request_body(ismael_server.requests().back()));
    EXPECT_EQ(body["messages"], Json::array({
        Json{{"role", "system"}, {"content", ismael_prompt}},
        Json{{"role", "user"},
             {"content",
              "Shared chat history (JSONL):\n"
              R"({"kind":"human","speaker":"Reader","addressed_to":"Cheburashka","text":"Who are you?"})"
              "\n"
              R"({"kind":"agent","speaker":"Cheburashka","text":"I am Cheburashka."})"}},
        Json{{"role", "user"}, {"content", "from Reader:\nand you?"}},
        Json{{"role", "assistant"}, {"content", "Call me Ismael."}},
        Json{{"role", "user"}, {"content", "from Reader:\nWhat did he say?"}},
    }));
}

} // namespace
} // namespace cha
