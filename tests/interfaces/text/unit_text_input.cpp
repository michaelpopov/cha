#include "agents/completion_backend.h"
#include "application/chat_coordinator.h"
#include "interfaces/text/text_input.h"
#include "application/session_database.h"
#include "support/test_backends.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace cha {
namespace {

class TemporaryTextSession {
public:
    TemporaryTextSession()
      : path(std::filesystem::temp_directory_path()
             / ("cha_text_input_"
                + std::to_string(
                    std::chrono::steady_clock::now()
                        .time_since_epoch()
                        .count())
                + ".sqlite3")) {
        if (!create_session_database(
                path,
                {
                    .id = "text-input-test",
                    .room = "test-room",
                    .label = "Text input test",
                })) {
            throw std::runtime_error(
                "Failed to create text-input test database");
        }
    }

    ~TemporaryTextSession() {
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }

    std::filesystem::path path;
};

AgentDefinition definition(
    std::string id = "guide-id",
    std::string name = "Guide") {
    return {
        .config = {
            .id = std::move(id),
            .name = std::move(name),
            .host = "127.0.0.1",
            .port = 8080,
        },
        .system_prompt = "Test prompt",
    };
}

class BlockingBackend final : public CompletionBackend {
public:
    RequestPayload prepare(
        const CompletionRequest& request,
        const ConversationReadView&) override {
        return {.bytes = request.prompt.text};
    }

    CompletionResult perform(
        RequestPayload,
        const CompletionDeltaSink&,
        const std::atomic_bool& cancellation) override {
        while (!cancellation.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return {CompletionOutcome::cancelled, {}};
    }

    AgentInfo info() const override {
        return {
            .id = id_,
            .name = "Guide",
            .model = "test-model",
            .api = "test://blocking",
            .streaming = true,
        };
    }

    const std::string& agent_id() const override {
        return id_;
    }

private:
    std::string id_{"guide-id"};
};

TEST(TextInput, DispatchesSlashCommandsAndOwnsExitSyntax) {
    TemporaryTextSession temporary;
    auto coordinator = ChatCoordinator::from_definitions(
        std::vector<AgentDefinition>{definition()},
        temporary.path);

    const CoordinatorUpdate invalid_argument =
        handle_text_input(*coordinator, "/clear later");
    EXPECT_TRUE(invalid_argument.clear_input);
    EXPECT_EQ(
        invalid_argument.notice,
        "Command does not accept arguments");
    const CoordinatorUpdate idle_stop_with_argument =
        handle_text_input(*coordinator, "/stop later");
    EXPECT_TRUE(idle_stop_with_argument.clear_input);
    EXPECT_EQ(
        idle_stop_with_argument.notice,
        "Command does not accept arguments");

    const CoordinatorUpdate unknown =
        handle_text_input(*coordinator, "/unknown");
    EXPECT_TRUE(unknown.clear_input);
    ASSERT_TRUE(unknown.notice);
    EXPECT_NE(unknown.notice->find("Unknown command"), std::string::npos);

    EXPECT_EQ(
        handle_text_input(*coordinator, "/clear").notice,
        "Conversation cleared");
    const CoordinatorUpdate information =
        handle_text_input(*coordinator, "/info");
    ASSERT_TRUE(information.notice);
    EXPECT_NE(
        information.notice->find("Transcript entries: 0"),
        std::string::npos);
    const CoordinatorUpdate agents =
        handle_text_input(*coordinator, "/agents");
    ASSERT_TRUE(agents.notice);
    EXPECT_NE(agents.notice->find("@Guide"), std::string::npos);
    EXPECT_EQ(
        handle_text_input(*coordinator, "/@Gui").notice,
        "Default agent is now Guide");

    const CoordinatorUpdate idle_stop =
        handle_text_input(*coordinator, "/stop");
    EXPECT_TRUE(idle_stop.clear_input);
    EXPECT_EQ(idle_stop.notice, "No generation is active");

    const CoordinatorUpdate exit =
        handle_text_input(*coordinator, "/exit");
    EXPECT_TRUE(exit.clear_input);
    EXPECT_TRUE(exit.end_session);
}

TEST(TextInput, ParsesAnAddressedPromptBeforeSubmission) {
    TemporaryTextSession temporary;
    auto coordinator = ChatCoordinator::from_definitions(
        std::vector<AgentDefinition>{
            definition(),
            definition("ismael-id", "Ismael"),
        },
        temporary.path);

    const CoordinatorUpdate submitted =
        handle_text_input(*coordinator, "  @Ism hello");
    EXPECT_TRUE(submitted.clear_input);
    const std::vector<ConversationEntry> entries =
        coordinator->conversation().entries();
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries.front().addressed_to, "ismael-id");
    EXPECT_EQ(entries.front().text, "hello");
    coordinator->shutdown();
}

TEST(TextInput, PreservesDraftsAndAcceptsStopDuringGeneration) {
    TemporaryTextSession temporary;
    auto coordinator = ChatCoordinator::from_backends_for_testing(
        test::one_backend(std::make_unique<BlockingBackend>()),
        temporary.path);

    (void)handle_text_input(*coordinator, "Question");
    const CoordinatorUpdate blocked =
        handle_text_input(*coordinator, "Another");
    EXPECT_FALSE(blocked.clear_input);
    EXPECT_EQ(
        blocked.notice,
        "Generation in progress; use /stop, Esc, or Ctrl-C");

    const CoordinatorUpdate stop_with_argument =
        handle_text_input(*coordinator, "/stop later");
    EXPECT_FALSE(stop_with_argument.clear_input);
    EXPECT_EQ(
        stop_with_argument.notice,
        "Generation in progress; use /stop, Esc, or Ctrl-C");

    const CoordinatorUpdate stopping =
        handle_text_input(*coordinator, "/stop");
    EXPECT_TRUE(stopping.clear_input);
    EXPECT_EQ(stopping.notice, "Stopping generation...");
    coordinator->shutdown();
}

} // namespace
} // namespace cha
