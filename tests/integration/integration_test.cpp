#include "characters/character.h"
#include "providers/providers.h"
#include "session/session_controller.h"
#include "characters/character_config.h"
#include "util/environment.h"
#include "support/mock_http_server.h"
#include "session/session_database.h"
#include "workspace/workspace.h"
#include "support/test_notifier.h"
#include "support/test_controller.h"
#include "support/test_session_database.h"
#include "support/test_workspace.h"
#include "web/r2_database_transfer.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
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

class ScopedEnvironmentVariable {
public:
    ScopedEnvironmentVariable(std::string name, std::string value)
        : name_(std::move(name)) {
        if (const char* current = std::getenv(name_.c_str())) previous_ = current;
        if (!previous_ || previous_->empty()) {
            if (!set_environment_variable(name_, value)) {
                throw std::runtime_error("Failed to set integration-test environment variable");
            }
        }
    }

    ~ScopedEnvironmentVariable() {
        if (previous_) {
            (void)set_environment_variable(name_, *previous_);
        } else {
            (void)unset_environment_variable(name_);
        }
    }

private:
    std::string name_;
    std::optional<std::string> previous_;
};

std::vector<TranscriptEntry> copy_entries(TranscriptView transcript) {
    const auto entries = transcript.entries;
    return {entries.begin(), entries.end()};
}

std::string file_bytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error(
            "Failed to read integration-test file '" + path.string() + "'");
    }
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
}

class TemporaryR2Database {
public:
    TemporaryR2Database()
        : directory_(
              std::filesystem::temp_directory_path()
              / ("cha_r2_integration_"
                 + std::to_string(
                     std::chrono::steady_clock::now()
                         .time_since_epoch().count()))),
          path_(directory_ / "cha-r2-integration-test.sqlite3") {
        std::filesystem::create_directories(directory_);
        (void)test::import_test_database(source_.root(), path_);
    }

    ~TemporaryR2Database() {
        std::error_code ignored;
        std::filesystem::remove_all(directory_, ignored);
    }

    const std::filesystem::path& path() const noexcept { return path_; }

private:
    test::TestWorkspace source_;
    std::filesystem::path directory_;
    std::filesystem::path path_;
};

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

CharacterDefinition integration_definition(bool stream) {
    const std::filesystem::path workspace_directory{CHA_WORKSPACE_DIRECTORY};
    load_dotenv(workspace_directory / ".env");
    const Workspace workspace = Workspace::load(workspace_directory);
    if (workspace.find_forum_member("lobby", "Ismael") == nullptr) {
        throw std::runtime_error("Checked-in workspace has no Ismael lobby member");
    }
    CharacterDefinition definition =
        workspace.character_definition("lobby", "Ismael");
    definition.provider.config.stream = stream;
    return definition;
}

GenerationEvent wait_for_generation_event(
    ProviderRequest& request,
    IntegrationDeadline deadline) {
    while (true) {
        const std::size_t observed = notifier().wake_count();
        GenerationEvent event = GenerationCompleted{};
        const ChannelReadStatus status = request.try_receive(event);
        if (status == ChannelReadStatus::value) {
            return event;
        }
        if (status == ChannelReadStatus::closed) {
            throw std::runtime_error(
                "Integration generation event queue closed unexpectedly");
        }

        const auto now = IntegrationClock::now();
        if (now >= deadline) {
            request.cancel();
            throw std::runtime_error(
                "Timed out after 60 seconds waiting for an integration generation "
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
            request.cancel();
            throw std::runtime_error(
                "Timed out after 60 seconds waiting for an integration generation "
                "event; cancelled the live request");
        }
    }
}

ChatResult run_chat(bool stream) {
    CharacterDefinition definition = integration_definition(stream);
    const CharacterMetadata target = definition.character;
    Transcript transcript;
    std::vector<CharacterDefinition> definitions;
    definitions.push_back(std::move(definition));
    const std::vector<SharedCharacterDefinition> shared =
        share_character_definitions(std::move(definitions));
    Providers providers;

    const std::string input = "Reply with one short sentence confirming that the connection works.";
    GenerationRequest request{
        .history = std::make_shared<const ModelHistory>(
            transcript.model_history()),
        .run = {
            .request_id = 1,
            .target = target,
            .prompt_text = input,
        },
    };
    auto wake = std::shared_ptr<WakeNotifier>(&notifier(), [](WakeNotifier*) {});
    const auto request_handle = providers.make_request(
        {.character = shared.front(), .generation = std::move(request)}, std::move(wake));

    ChatResult result;
    const IntegrationDeadline deadline =
        IntegrationClock::now() + integration_chat_timeout;
    while (true) {
        const GenerationEvent event = wait_for_generation_event(*request_handle, deadline);
        if (const auto* delta = std::get_if<GenerationEventDelta>(&event)) {
            ++result.chunks;
            result.response += delta->text;
        } else {
            EXPECT_TRUE(std::holds_alternative<GenerationCompleted>(event));
            break;
        }
    }

    providers.shutdown();
    return result;
}

ChatResult run_cancelled_chat() {
    CharacterDefinition definition = integration_definition(true);
    const CharacterMetadata target = definition.character;
    Transcript transcript;
    std::vector<CharacterDefinition> definitions;
    definitions.push_back(std::move(definition));
    const std::vector<SharedCharacterDefinition> shared =
        share_character_definitions(std::move(definitions));
    Providers providers;

    const std::string input = "Write a detailed essay of at least two thousand words about distributed systems.";
    GenerationRequest request{
        .history = std::make_shared<const ModelHistory>(
            transcript.model_history()),
        .run = {
            .request_id = 2,
            .target = target,
            .prompt_text = input,
        },
    };
    auto wake = std::shared_ptr<WakeNotifier>(&notifier(), [](WakeNotifier*) {});
    const auto request_handle = providers.make_request(
        {.character = shared.front(), .generation = std::move(request)}, std::move(wake));

    ChatResult result;
    const IntegrationDeadline deadline =
        IntegrationClock::now() + integration_chat_timeout;
    while (true) {
        const GenerationEvent event = wait_for_generation_event(*request_handle, deadline);
        if (const auto* delta = std::get_if<GenerationEventDelta>(&event)) {
            ++result.chunks;
            result.response += delta->text;
            request_handle->cancel();
        } else {
            EXPECT_TRUE(std::holds_alternative<GenerationCancelled>(event));
            break;
        }
    }

    providers.shutdown();
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

// This test intentionally overwrites one dedicated object named
// cha-r2-integration-test.sqlite3 in the bucket selected by CHA_R2_URL.
TEST(R2Integration, UploadsDownloadsAndBacksUpThePreviousDatabase) {
    TemporaryR2Database fixture;

    const web::R2DatabaseTransfer uploaded =
        web::upload_database_to_r2(fixture.path());
    const std::string expected_download = file_bytes(fixture.path());
    ASSERT_EQ(uploaded.byte_count, expected_download.size());

    test::TestWorkspace previous_local;
    previous_local.add_persona("r2test", "R2 test persona");
    (void)test::import_test_database(previous_local.root(), fixture.path());
    const std::string expected_backup = file_bytes(fixture.path());
    ASSERT_NE(expected_backup, expected_download);

    const web::R2DatabaseTransfer downloaded =
        web::download_database_from_r2(fixture.path());
    std::filesystem::path backup = fixture.path();
    backup += ".bac";

    EXPECT_EQ(downloaded.byte_count, expected_download.size());
    EXPECT_EQ(file_bytes(fixture.path()), expected_download);
    EXPECT_EQ(file_bytes(backup), expected_backup);
}

// Removes one temporary session database when a multi-character test leaves scope.
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
                {.id = "multi-character", .forum = "lobby", .label = "Multi-character"})) {
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
    std::vector<CharacterDefinition> definitions;
    PersonaRoster personas;
    std::string author_id;
    std::string author_name;
};

LobbySetup lobby_setup() {
    const std::filesystem::path root{CHA_WORKSPACE_DIRECTORY};
    // The mock transports below replace every loaded provider before use, but
    // workspace loading correctly validates every referenced credential name.
    ScopedEnvironmentVariable openai_key("OPENAI_API_KEY", "integration-test-key");
    ScopedEnvironmentVariable openrouter_key("OPEN_ROUTER_API_KEY", "integration-test-key");
    ScopedEnvironmentVariable gemini_key("GEMINI_API_KEY", "integration-test-key");
    const Workspace workspace = Workspace::load(root);
    const WorkspaceForum* const forum = workspace.find_forum("lobby");
    if (forum == nullptr) throw std::runtime_error("Checked-in workspace has no lobby forum");
    const WorkspacePersona* const configured_persona =
        workspace.find_persona(forum->default_persona_id);
    const Persona* persona = nullptr;
    PersonaRoster personas;
    if (configured_persona != nullptr) {
        personas.push_back(*configured_persona);
        persona = &personas.front();
    }
    if (persona == nullptr) throw std::runtime_error("Lobby default persona is not in the roster");
    std::vector<CharacterDefinition> definitions;
    definitions.reserve(forum->members.size());
    for (const WorkspaceForumMember& member : forum->members) {
        definitions.push_back(
            workspace.character_definition(forum->id, member.character_id));
    }
    return {
        .definitions = std::move(definitions),
        .personas = personas,
        .author_id = persona->id,
        .author_name = persona->display_name,
    };
}

std::string current_system_prompt(std::string_view character_id) {
    const std::shared_ptr<const Workspace> workspace = getws();
    const WorkspaceForumMember* const member =
        workspace == nullptr
        ? nullptr : workspace->find_forum_member("lobby", character_id);
    if (member == nullptr) {
        throw std::runtime_error(
            "Published test workspace has no lobby member '"
            + std::string(character_id) + "'");
    }
    return member->system_prompt;
}

// Redirects one character's backend at a local mock server without touching its prompt.
void point_at(CharacterDefinition& definition, int port) {
    definition.provider.config.host = "127.0.0.1";
    definition.provider.config.port = port;
    definition.provider.config.https = false;
    definition.provider.config.mode = Mode::net;
    // The local fixtures below use the Chat Completions wire format. The
    // checked-in workspace defaults to Responses, so make the test transport
    // choice explicit rather than relying on the workspace default.
    definition.provider.config.api = ProviderApi::chat_completions;
    definition.provider.config.web_search = WebSearchMode::off;
    definition.provider.config.stream = false;
    // The local mock server does not require authentication.
    definition.provider.config.api_key_env.clear();
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

Json messages_without_timestamps(Json messages) {
    constexpr std::string_view created_at = R"(,"created_at":")";
    for (Json& message : messages) {
        std::string& content = message.at("content").get_ref<std::string&>();

        // Ordinary human and character messages render their timestamps as
        // text around the message body.
        if (content.starts_with("from ")) {
            const std::size_t at = content.find(" at ");
            if (at != std::string::npos
                && content.size() > at + 25
                && content.compare(at + 24, 2, ":\n") == 0) {
                content.erase(at, 24);
            }
        } else if (content.size() > 22
            && content.front() == '['
            && content[21] == ']'
            && content[22] == '\n') {
            content.erase(0, 23);
        }

        // Shared-history entries carry the same value as a JSON field.
        std::size_t field = content.find(created_at);
        while (field != std::string::npos) {
            const std::size_t value_end = content.find(
                '"', field + created_at.size());
            if (value_end == std::string::npos) break;
            content.erase(field, value_end - field + 1);
            field = content.find(created_at, field);
        }
    }
    return messages;
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
    std::vector<CharacterDefinition>& definitions = lobby.definitions;
    definitions.resize(1);
    constexpr std::string_view reasoning_marker =
        "INTEGRATION_PRIVATE_REASONING_731";
    MockHttpServer server({
        streamed_answer(reasoning_marker, "First answer"),
        streamed_answer("SECOND_PRIVATE_REASONING", "Second answer"),
    });
    server.start();
    point_at(definitions.front(), server.port());
    definitions.front().provider.config.stream = true;
    definitions.front().provider.config.reasoning_format =
        ReasoningFormat::automatic;

    TemporarySession session;
    {
        auto controller = test::from_test_workspace(
            std::move(definitions),
            lobby.personas,
            session.path,
            notifier());
        (void)controller->submit_prompt(lobby.author_id, "First question");
        run_until_idle(*controller);
        const std::vector<TranscriptEntry> live =
            copy_entries(controller->view().transcript);
        ASSERT_EQ(live.size(), 2U);
        EXPECT_EQ(live.back().text, "First answer");

        (void)controller->submit_prompt(lobby.author_id, "Second question");
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
    std::vector<CharacterDefinition>& definitions = lobby.definitions;
    definitions.resize(1);
    MockHttpServer server({http_response(
        "application/json",
        R"({"choices":[{"message":{"reasoning":"Non-stream thought","content":"Non-stream answer"}}]})")});
    server.start();
    point_at(definitions.front(), server.port());
    definitions.front().provider.config.reasoning_format =
        ReasoningFormat::reasoning;

    TemporarySession session;
    auto controller = test::from_test_workspace(
        std::move(definitions),
        lobby.personas,
        session.path,
        notifier());
    (void)controller->submit_prompt(lobby.author_id, "Question");
    run_until_idle(*controller);
    server.join();

    const std::vector<TranscriptEntry> live =
        copy_entries(controller->view().transcript);
    ASSERT_EQ(live.size(), 2U);
    EXPECT_EQ(live.back().text, "Non-stream answer");
    const std::vector<TranscriptEntry> restored =
        load_transcript_entries(session.path);
    EXPECT_EQ(restored.back().text, "Non-stream answer");
}

TEST(CoverIntegration, OmitsCoveredTurnsFromTheSerializedNextRequest) {
    LobbySetup lobby = lobby_setup();
    std::vector<CharacterDefinition>& definitions = lobby.definitions;
    definitions.resize(1);
    std::string system_prompt;
    MockHttpServer server({
        answer("Visible answer"),
        answer("Hidden answer"),
        answer("Current answer"),
    });
    server.start();
    point_at(definitions.front(), server.port());

    TemporarySession session;
    {
        auto controller = test::from_test_workspace(
            std::move(definitions),
            lobby.personas,
            session.path,
            notifier());
        system_prompt = current_system_prompt("Cheburashka");
        (void)controller->submit_prompt(lobby.author_id, "Visible question");
        run_until_idle(*controller);
        EXPECT_TRUE(has_state_update(controller->cover_conversation()));
        (void)controller->submit_prompt(lobby.author_id, "Hidden question");
        run_until_idle(*controller);
        EXPECT_TRUE(has_state_update(controller->cover_conversation()));
        (void)controller->submit_prompt(lobby.author_id, "Current question");
        run_until_idle(*controller);
    }
    server.join();

    ASSERT_EQ(server.requests().size(), 3U);
    const Json current_body =
        Json::parse(request_body(server.requests().back()));
    EXPECT_EQ(messages_without_timestamps(current_body["messages"]), Json::array({
        Json{{"role", "system"}, {"content", system_prompt}},
        Json{{"role", "user"}, {"content", "from " + lobby.author_name + ":\nCurrent question"}},
    }));

    const std::vector<TranscriptEntry> restored =
        load_transcript_entries(session.path);
    ASSERT_EQ(restored.size(), 6U);
    EXPECT_EQ(restored[2].text, "Hidden question");
    EXPECT_EQ(restored[3].text, "Hidden answer");
}

TEST(MultiCharacterIntegration, RoutesEachPromptToItsOwnCharacterOverItsOwnTransport) {
    LobbySetup lobby = lobby_setup();
    std::vector<CharacterDefinition>& definitions = lobby.definitions;
    ASSERT_EQ(definitions.size(), 2U);
    ASSERT_EQ(definitions.front().character.display_name, "Cheburashka");
    ASSERT_EQ(definitions.back().character.display_name, "Ismael");
    std::string cheburashka_prompt;
    std::string ismael_prompt;

    MockHttpServer cheburashka_server({answer("I am Cheburashka.")});
    MockHttpServer ismael_server({answer("Call me Ismael.")});
    cheburashka_server.start();
    ismael_server.start();
    point_at(definitions.front(), cheburashka_server.port());
    point_at(definitions.back(), ismael_server.port());

    TemporarySession session;
    {
        auto controller = test::from_test_workspace(
            std::move(definitions),
            lobby.personas,
            session.path,
            notifier());
        cheburashka_prompt = current_system_prompt("Cheburashka");
        ismael_prompt = current_system_prompt("Ismael");
        ASSERT_NE(cheburashka_prompt, ismael_prompt);
        ASSERT_EQ(
            getws()->find_forum("lobby")->members.front().character_id,
            "Cheburashka");

        // No mention: the first character directory in name order answers.
        ControllerUpdate update =
            controller->submit_prompt(lobby.author_id, "Who are you?");
        ASSERT_TRUE(update.input_consumed);
        run_until_idle(*controller);

        // An addressed prompt reaches the mentioned character instead.
        update = controller->submit_prompt(lobby.author_id, "and you?", "Ismael");
        ASSERT_TRUE(update.input_consumed);
        run_until_idle(*controller);
    }
    cheburashka_server.join();
    ismael_server.join();

    ASSERT_EQ(cheburashka_server.requests().size(), 1U)
        << "the mentioned turn must not reach the default character";
    ASSERT_EQ(ismael_server.requests().size(), 1U);

    const Json first = body_of(cheburashka_server);
    EXPECT_EQ(messages_without_timestamps(first["messages"]), Json::array({
        Json{{"role", "system"}, {"content", cheburashka_prompt}},
        Json{{"role", "user"}, {"content", "from " + lobby.author_name + ":\nWho are you?"}},
    }));

    // Ismael's own system prompt, and Cheburashka's answer attributed as
    // user-role forum context.
    const Json second = body_of(ismael_server);
    EXPECT_EQ(messages_without_timestamps(second["messages"]), Json::array({
        Json{{"role", "system"}, {"content", ismael_prompt}},
        Json{{"role", "user"},
             {"content",
              "Shared chat history (JSONL):\n"
              "{\"kind\":\"human\",\"speaker\":\"" + lobby.author_name
              + "\",\"addressed_to\":\"Cheburashka\",\"text\":\"Who are you?\"}"
              "\n"
              R"({"kind":"character","speaker":"Cheburashka","text":"I am Cheburashka."})"}},
        Json{{"role", "user"}, {"content", "from " + lobby.author_name + ":\nand you?"}},
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

TEST(MultiCharacterIntegration, MulticastSendsIndependentBodiesAndRestoresHistory) {
    LobbySetup lobby = lobby_setup();
    std::vector<CharacterDefinition>& definitions = lobby.definitions;
    ASSERT_EQ(definitions.size(), 2U);
    std::string cheburashka_prompt;
    std::string ismael_prompt;
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
        auto controller = test::from_test_workspace(
            std::move(definitions), lobby.personas, session.path,
            notifier());
        cheburashka_prompt = current_system_prompt("Cheburashka");
        ismael_prompt = current_system_prompt("Ismael");
        const ControllerUpdate multicast = controller->start_multicast(
            lobby.author_id, "What time is it?", {});
        ASSERT_TRUE(multicast.input_consumed);
        run_until_idle(*controller);

        (void)controller->submit_prompt(lobby.author_id, "What did the panel say?");
        run_until_idle(*controller);
    }
    cheburashka_server.join();
    ismael_server.join();

    ASSERT_EQ(cheburashka_server.requests().size(), 2U);
    ASSERT_EQ(ismael_server.requests().size(), 1U);
    EXPECT_EQ(
        messages_without_timestamps(
            Json::parse(request_body(cheburashka_server.requests()[0]))["messages"]),
        Json::array({
            Json{{"role", "system"}, {"content", cheburashka_prompt}},
        Json{{"role", "user"}, {"content", "from " + lobby.author_name + ":\nWhat time is it?"}},
        }));
    EXPECT_EQ(
        messages_without_timestamps(body_of(ismael_server)["messages"]),
        Json::array({
            Json{{"role", "system"}, {"content", ismael_prompt}},
        Json{{"role", "user"}, {"content", "from " + lobby.author_name + ":\nWhat time is it?"}},
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

TEST(MultiCharacterIntegration, ReopensTheSessionWhenTheForumKeepsOnlyOneCharacter) {
    LobbySetup lobby = lobby_setup();
    std::vector<CharacterDefinition>& definitions = lobby.definitions;
    std::string ismael_prompt;

    MockHttpServer cheburashka_server({answer("I am Cheburashka.")});
    MockHttpServer ismael_server({answer("Call me Ismael."), answer("He greeted you.")});
    cheburashka_server.start();
    ismael_server.start();
    point_at(definitions.front(), cheburashka_server.port());
    point_at(definitions.back(), ismael_server.port());
    CharacterDefinition ismael_only = definitions.back();

    TemporarySession session;
    {
        auto controller = test::from_test_workspace(
            std::move(definitions),
            lobby.personas,
            session.path,
            notifier());
        (void)controller->submit_prompt(lobby.author_id, "Who are you?");
        run_until_idle(*controller);
        (void)controller->submit_prompt(lobby.author_id, "and you?", "Ismael");
        run_until_idle(*controller);
    }
    cheburashka_server.join();

    // Cheburashka has left the forum; the stored session still opens.
    SessionRestore restored = load_session_state(session.path);
    ASSERT_EQ(restored.entries.size(), 4U);
    ASSERT_TRUE(restored.interrupted_turns.empty());

    auto reopened = test::from_test_workspace(
        std::vector<CharacterDefinition>{std::move(ismael_only)},
        lobby.personas,
        session.path,
        notifier(),
        std::move(restored));
    ismael_prompt = current_system_prompt("Ismael");
    ASSERT_NE(getws()->find_forum("lobby"), nullptr);
    EXPECT_EQ(getws()->find_forum("lobby")->members.size(), 1U);
    EXPECT_EQ(
        reopened->submit_prompt(
            lobby.author_id, "are you there?", "Cheburashka").notice,
        "Unknown character @Cheburashka. Characters in this forum: @Ismael");

    (void)reopened->submit_prompt(lobby.author_id, "What did he say?");
    run_until_idle(*reopened);
    ismael_server.join();

    ASSERT_EQ(ismael_server.requests().size(), 2U);
    const Json body = Json::parse(request_body(ismael_server.requests().back()));
    EXPECT_EQ(messages_without_timestamps(body["messages"]), Json::array({
        Json{{"role", "system"}, {"content", ismael_prompt}},
        Json{{"role", "user"},
             {"content",
              "Shared chat history (JSONL):\n"
              "{\"kind\":\"human\",\"speaker\":\"" + lobby.author_name
              + "\",\"addressed_to\":\"Cheburashka\",\"text\":\"Who are you?\"}"
              "\n"
              R"({"kind":"character","speaker":"Cheburashka","text":"I am Cheburashka."})"}},
        Json{{"role", "user"}, {"content", "from " + lobby.author_name + ":\nand you?"}},
        Json{{"role", "assistant"}, {"content", "Call me Ismael."}},
        Json{{"role", "user"}, {"content", "from " + lobby.author_name + ":\nWhat did he say?"}},
    }));
}

} // namespace
} // namespace cha
