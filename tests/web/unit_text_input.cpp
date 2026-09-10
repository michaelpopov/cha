#include "providers/model_backend.h"
#include "session/session_controller.h"
#include "web/text_input.h"
#include "session/session_database.h"
#include "support/test_backends.h"
#include "support/test_controller.h"
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

namespace cha::web {
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

CharacterDefinition definition(
    std::string id = "guide-id",
    std::string name = "Guide") {
    return {
        .character = {
            .id = std::move(id),
            .display_name = std::move(name),
        },
        .provider = {.id = "test", .config = {
            .host = "127.0.0.1",
            .port = 8080,
            .model = "fake",
        }},
        .system_prompt = "Test prompt",
    };
}

std::vector<TranscriptEntry> copy_entries(TranscriptView transcript) {
    const auto entries = transcript.entries;
    return {entries.begin(), entries.end()};
}

class BlockingBackend final : public test::DescribedModelBackend {
public:
    RequestPayload prepare(const GenerationRequest& input) override {
        return {.bytes = input.run.prompt_text};
    }

    GenerationResult perform(
        RequestPayload,
        const GenerationDeltaSink&,
        const std::atomic_bool& cancellation) override {
        while (!cancellation.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        return {GenerationOutcome::cancelled, {}};
    }

    test::DescribedBackendInfo info() const override {
        return {
            .character = {
                .id = id_,
                .display_name = "Guide",
            },
            .model = "test-model",
            .api = "test://blocking",
            .streaming = true,
        };
    }

private:
    std::string id_{"guide-id"};
};

TEST(TextInput, DispatchesTheRemainingSlashCommands) {
    TemporaryTextSession temporary;
    auto controller = test::from_test_workspace(
        std::vector<CharacterDefinition>{definition()},
        temporary.path,
        notifier());

    for (const std::string_view removed : {
             "/clear", "/info", "/characters", "/agents", "/@Guide",
             "/style sans-bold", "/cover", "/uncover", "/stop", "/exit"}) {
        const CommandResult result =
            handle_text_input(*controller, "operator", std::string(removed));
        EXPECT_TRUE(result.clear_input) << removed;
        ASSERT_TRUE(result.session.notice) << removed;
        EXPECT_NE(result.session.notice->find("Unknown command"), std::string::npos)
            << removed;
    }

    const CommandResult empty_multicast =
        handle_text_input(*controller, "operator", "/mcast");
    EXPECT_TRUE(empty_multicast.clear_input);
    EXPECT_EQ(empty_multicast.session.notice, "Multicast prompt is empty");
}

TEST(TextInput, ParsesAnAddressedPromptBeforeSubmission) {
    TemporaryTextSession temporary;
    auto controller = test::from_test_workspace(
        std::vector<CharacterDefinition>{
            definition(),
            definition("ismael-id", "Ismael"),
        },
        temporary.path,
        notifier());

    const CommandResult submitted =
        handle_text_input(*controller, "operator", "  @Ism hello");
    EXPECT_TRUE(submitted.clear_input);
    const std::vector<TranscriptEntry> entries =
        copy_entries(controller->view().transcript);
    ASSERT_EQ(entries.size(), 1U);
    EXPECT_EQ(entries.front().addressed_to, "ismael-id");
    EXPECT_EQ(entries.front().text, "hello");
    controller->shutdown();
}

TEST(TextInput, RecordsAnInlineSelfNote) {
    TemporaryTextSession temporary;
    auto controller = test::from_test_workspace(
        std::vector<CharacterDefinition>{definition()},
        temporary.path,
        notifier());

    // `@- text` records one message with no reply and clears the input.
    const CommandResult recorded =
        handle_text_input(*controller, "operator", "@- thinking out loud");
    EXPECT_TRUE(recorded.clear_input);
    ASSERT_EQ(controller->view().transcript.entries.size(), 1U);
    EXPECT_EQ(
        controller->view().transcript.entries.front().addressed_to, "-");
    EXPECT_EQ(
        controller->view().transcript.entries.front().text,
        "thinking out loud");

    controller->shutdown();
}

TEST(TextInput, ForwardsAuthorOnlyToBatchStartingCommands) {
    TemporaryTextSession ordinary_temporary;
    auto ordinary_controller = test::from_test_backends(
        test::one_backend(std::make_unique<BlockingBackend>()),
        PersonaRoster{{.id = "engineer", .display_name = "Engineer"}},
        ordinary_temporary.path,
        notifier());

    const CommandResult ordinary =
        handle_text_input(*ordinary_controller, "engineer", "Question");
    EXPECT_TRUE(ordinary.clear_input);
    ASSERT_EQ(ordinary_controller->view().transcript.entries.size(), 1U);
    EXPECT_EQ(
        ordinary_controller->view().transcript.entries.front().participant_id,
        "engineer");
    EXPECT_EQ(
        ordinary_controller->view().transcript.entries.front().display_name,
        "Engineer");
    ordinary_controller->shutdown();

    TemporaryTextSession multicast_temporary;
    auto multicast_controller = test::from_test_backends(
        test::one_backend(std::make_unique<BlockingBackend>()),
        PersonaRoster{{.id = "engineer", .display_name = "Engineer"}},
        multicast_temporary.path,
        notifier());

    const CommandResult multicast = handle_text_input(
        *multicast_controller, "engineer", "/mcast @Guide Question");
    EXPECT_TRUE(multicast.clear_input);
    ASSERT_EQ(multicast_controller->view().transcript.entries.size(), 1U);
    EXPECT_EQ(
        multicast_controller->view().transcript.entries.front().participant_id,
        "engineer");
    EXPECT_EQ(
        multicast_controller->view().transcript.entries.front().display_name,
        "Engineer");
    multicast_controller->shutdown();
}

TEST(TextInput, DelegatesMulticastRecipientResolutionBeforeStartingAnyChild) {
    TemporaryTextSession temporary;
    auto controller = test::from_test_workspace(
        std::vector<CharacterDefinition>{definition()},
        temporary.path,
        notifier());

    const CommandResult duplicate = handle_text_input(
        *controller, "operator", "/mcast @Guide @Gui What time is it?");
    EXPECT_TRUE(duplicate.clear_input);
    EXPECT_EQ(duplicate.session.notice, "Multicast target @Guide is duplicated");
    EXPECT_TRUE(controller->view().transcript.entries.empty());

    const CommandResult unknown = handle_text_input(
        *controller, "operator", "/mcast @Nobody What time is it?");
    EXPECT_TRUE(unknown.clear_input);
    ASSERT_TRUE(unknown.session.notice);
    EXPECT_NE(unknown.session.notice->find("Unknown character @Nobody"), std::string::npos);
    EXPECT_TRUE(controller->view().transcript.entries.empty());
}

TEST(TextInput, PreservesDraftsDuringGeneration) {
    TemporaryTextSession temporary;
    auto controller = test::from_test_backends(
        test::one_backend(std::make_unique<BlockingBackend>()),
        temporary.path,
        notifier());

    (void)handle_text_input(*controller, "operator", "Question");
    const CommandResult blocked =
        handle_text_input(*controller, "operator", "Another");
    EXPECT_FALSE(blocked.clear_input);
    EXPECT_EQ(
        blocked.session.notice,
        "Generation in progress; use the Stop button");

    const CommandResult multicast_while_active =
        handle_text_input(*controller, "operator", "/mcast Question");
    EXPECT_FALSE(multicast_while_active.clear_input);
    EXPECT_EQ(
        multicast_while_active.session.notice,
        "Generation in progress; use the Stop button");

    const CommandResult stop_with_argument =
        handle_text_input(*controller, "operator", "/stop later");
    EXPECT_FALSE(stop_with_argument.clear_input);
    EXPECT_EQ(
        stop_with_argument.session.notice,
        "Generation in progress; use the Stop button");

    const CommandResult stopping =
        handle_text_input(*controller, "operator", "/stop");
    EXPECT_FALSE(stopping.clear_input);
    EXPECT_EQ(stopping.session.notice, "Generation in progress; use the Stop button");
    (void)controller->request_stop();
    controller->shutdown();
}

TEST(TextInput, SeparatesDraftClearingFromControllerAcceptance) {
    TemporaryTextSession temporary;
    auto controller = test::from_test_workspace(
        std::vector<CharacterDefinition>{definition()}, temporary.path, notifier());

    const CommandResult unknown_author =
        handle_text_input(*controller, "unknown", "Question");
    EXPECT_FALSE(unknown_author.session.input_consumed);
    EXPECT_FALSE(unknown_author.clear_input);

    const CommandResult removed_command =
        handle_text_input(*controller, "operator", "/clear later");
    EXPECT_FALSE(removed_command.session.input_consumed);
    EXPECT_TRUE(removed_command.clear_input);

    const CommandResult rejected_multicast =
        handle_text_input(*controller, "operator", "/mcast");
    EXPECT_TRUE(rejected_multicast.clear_input);
    EXPECT_FALSE(has_state_update(rejected_multicast.session));

    controller->shutdown();
    const CommandResult undispatchable =
        handle_text_input(*controller, "operator", "Another question");
    EXPECT_FALSE(undispatchable.session.input_consumed);
    EXPECT_FALSE(undispatchable.clear_input);
}

} // namespace
} // namespace cha::web
