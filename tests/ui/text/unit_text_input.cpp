#include "agents/completion_backend.h"
#include "session/session_controller.h"
#include "ui/text/text_input.h"
#include "session/session_database.h"
#include "support/test_backends.h"
#include "support/test_notifier.h"

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

test::NoopNotifier& notifier() {
    static test::NoopNotifier instance;
    return instance;
}

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
                    .forum = "test-forum",
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

std::vector<TranscriptEntry> copy_entries(const Transcript& transcript) {
    const auto entries = transcript.entries();
    return {entries.begin(), entries.end()};
}

class BlockingBackend final : public CompletionBackend {
public:
    RequestPayload prepare(const CompletionInput& input) override {
        return {.bytes = input.run.prompt_text};
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

    AgentRuntimeInfo info() const override {
        return {
            .persona = {
                .id = id_,
                .name = "Guide",
            },
            .model = "test-model",
            .api = "test://blocking",
            .streaming = true,
        };
    }

private:
    std::string id_{"guide-id"};
};

TEST(TextInput, DispatchesSlashCommandsAndOwnsExitSyntax) {
    TemporaryTextSession temporary;
    auto controller = SessionController::from_definitions_for_testing(
        std::vector<AgentDefinition>{definition()},
        temporary.path,
        notifier());

    const SessionUpdate invalid_argument =
        handle_text_input(*controller, "operator", "/clear later");
    EXPECT_TRUE(invalid_argument.clear_input);
    EXPECT_EQ(
        invalid_argument.notice,
        "Command does not accept arguments");
    const SessionUpdate idle_stop_with_argument =
        handle_text_input(*controller, "operator", "/stop later");
    EXPECT_TRUE(idle_stop_with_argument.clear_input);
    EXPECT_EQ(
        idle_stop_with_argument.notice,
        "Command does not accept arguments");

    const SessionUpdate unknown =
        handle_text_input(*controller, "operator", "/unknown");
    EXPECT_TRUE(unknown.clear_input);
    ASSERT_TRUE(unknown.notice);
    EXPECT_NE(unknown.notice->find("Unknown command"), std::string::npos);
    EXPECT_NE(unknown.notice->find("/mcast"), std::string::npos);

    const SessionUpdate empty_multicast =
        handle_text_input(*controller, "operator", "/mcast");
    EXPECT_TRUE(empty_multicast.clear_input);
    EXPECT_EQ(empty_multicast.notice, "Multicast prompt is empty");

    EXPECT_EQ(
        handle_text_input(*controller, "not-a-user", "/clear").notice,
        "Transcript cleared");
    EXPECT_TRUE(handle_text_input(*controller, "operator", "/hide-on").render_needed);
    EXPECT_TRUE(handle_text_input(*controller, "operator", "/hide").render_needed);
    EXPECT_TRUE(handle_text_input(*controller, "operator", "/hide-off").render_needed);
    EXPECT_EQ(
        copy_entries(controller->transcript()),
        (std::vector<TranscriptEntry>{
            make_hide_on_marker(1),
            make_hide_marker(2),
            make_hide_off_marker(3),
        }));
    const SessionUpdate information =
        handle_text_input(*controller, "operator", "/info");
    ASSERT_TRUE(information.notice);
    EXPECT_NE(
        information.notice->find("Transcript entries: 3"),
        std::string::npos);
    const SessionUpdate agents =
        handle_text_input(*controller, "operator", "/agents");
    ASSERT_TRUE(agents.notice);
    EXPECT_NE(agents.notice->find("@Guide"), std::string::npos);
    EXPECT_EQ(
        handle_text_input(*controller, "operator", "/@Gui").notice,
        "Default agent is now Guide");

    const SessionUpdate idle_stop =
        handle_text_input(*controller, "operator", "/stop");
    EXPECT_TRUE(idle_stop.clear_input);
    EXPECT_EQ(idle_stop.notice, "No generation is active");

    const SessionUpdate exit =
        handle_text_input(*controller, "operator", "/exit");
    EXPECT_TRUE(exit.clear_input);
    EXPECT_TRUE(exit.end_session);
}

TEST(TextInput, ParsesAnAddressedPromptBeforeSubmission) {
    TemporaryTextSession temporary;
    auto controller = SessionController::from_definitions_for_testing(
        std::vector<AgentDefinition>{
            definition(),
            definition("ismael-id", "Ismael"),
        },
        temporary.path,
        notifier());

    const SessionUpdate submitted =
        handle_text_input(*controller, "operator", "  @Ism hello");
    EXPECT_TRUE(submitted.clear_input);
    const std::vector<TranscriptEntry> entries =
        copy_entries(controller->transcript());
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries.front().addressed_to, "ismael-id");
    EXPECT_EQ(entries.front().text, "hello");
    controller->shutdown();
}

TEST(TextInput, ForwardsAuthorOnlyToBatchStartingCommands) {
    TemporaryTextSession ordinary_temporary;
    auto ordinary_controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<BlockingBackend>()),
        UserRoster{{.id = "engineer", .display_name = "Engineer"}},
        ordinary_temporary.path,
        notifier());

    const SessionUpdate ordinary =
        handle_text_input(*ordinary_controller, "engineer", "Question");
    EXPECT_TRUE(ordinary.clear_input);
    ASSERT_EQ(ordinary_controller->transcript().entries().size(), 1U);
    EXPECT_EQ(
        ordinary_controller->transcript().entries().front().participant_id,
        "engineer");
    EXPECT_EQ(
        ordinary_controller->transcript().entries().front().display_name,
        "Engineer");
    ordinary_controller->shutdown();

    TemporaryTextSession multicast_temporary;
    auto multicast_controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<BlockingBackend>()),
        UserRoster{{.id = "engineer", .display_name = "Engineer"}},
        multicast_temporary.path,
        notifier());

    const SessionUpdate multicast = handle_text_input(
        *multicast_controller, "engineer", "/mcast @Guide Question");
    EXPECT_TRUE(multicast.clear_input);
    ASSERT_EQ(multicast_controller->transcript().entries().size(), 1U);
    EXPECT_EQ(
        multicast_controller->transcript().entries().front().participant_id,
        "engineer");
    EXPECT_EQ(
        multicast_controller->transcript().entries().front().display_name,
        "Engineer");
    multicast_controller->shutdown();
}

TEST(TextInput, DelegatesMulticastRecipientResolutionBeforeStartingAnyChild) {
    TemporaryTextSession temporary;
    auto controller = SessionController::from_definitions_for_testing(
        std::vector<AgentDefinition>{definition()},
        temporary.path,
        notifier());

    const SessionUpdate duplicate = handle_text_input(
        *controller, "operator", "/mcast @Guide @Gui What time is it?");
    EXPECT_TRUE(duplicate.clear_input);
    EXPECT_EQ(duplicate.notice, "Multicast target @Guide is duplicated");
    EXPECT_TRUE(controller->transcript().entries().empty());

    const SessionUpdate unknown = handle_text_input(
        *controller, "operator", "/mcast @Nobody What time is it?");
    EXPECT_TRUE(unknown.clear_input);
    ASSERT_TRUE(unknown.notice);
    EXPECT_NE(unknown.notice->find("Unknown agent @Nobody"), std::string::npos);
    EXPECT_TRUE(controller->transcript().entries().empty());
}

TEST(TextInput, PreservesDraftsAndAcceptsStopDuringGeneration) {
    TemporaryTextSession temporary;
    auto controller = SessionController::from_backends_for_testing(
        test::one_backend(std::make_unique<BlockingBackend>()),
        temporary.path,
        notifier());

    (void)handle_text_input(*controller, "operator", "Question");
    const SessionUpdate blocked =
        handle_text_input(*controller, "operator", "Another");
    EXPECT_FALSE(blocked.clear_input);
    EXPECT_EQ(
        blocked.notice,
        "Generation in progress; use /stop, Esc, or Ctrl-C");

    const SessionUpdate hidden_while_active =
        handle_text_input(*controller, "operator", "/hide");
    EXPECT_FALSE(hidden_while_active.clear_input);
    EXPECT_EQ(
        hidden_while_active.notice,
        "Generation in progress; use /stop, Esc, or Ctrl-C");

    const SessionUpdate stop_with_argument =
        handle_text_input(*controller, "operator", "/stop later");
    EXPECT_FALSE(stop_with_argument.clear_input);
    EXPECT_EQ(
        stop_with_argument.notice,
        "Generation in progress; use /stop, Esc, or Ctrl-C");

    const SessionUpdate stopping =
        handle_text_input(*controller, "operator", "/stop");
    EXPECT_TRUE(stopping.clear_input);
    EXPECT_EQ(stopping.notice, "Stopping generation...");
    controller->shutdown();
}

} // namespace
} // namespace cha
