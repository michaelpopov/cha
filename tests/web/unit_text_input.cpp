#include "agents/completion_backend.h"
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
        .completion = {
            .host = "127.0.0.1",
            .port = 8080,
        },
        .system_prompt = "Test prompt",
    };
}

std::vector<TranscriptEntry> copy_entries(TranscriptView transcript) {
    const auto entries = transcript.entries;
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

    CompletionBackendInfo info() const override {
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

TEST(TextInput, DispatchesSlashCommandsAndOwnsExitSyntax) {
    TemporaryTextSession temporary;
    auto controller = test::from_definitions_for_testing(
        std::vector<CharacterDefinition>{definition()},
        temporary.path,
        notifier());

    const CommandResult invalid_argument =
        handle_text_input(*controller, "operator", "/clear later");
    EXPECT_TRUE(invalid_argument.clear_input);
    EXPECT_EQ(
        invalid_argument.session.notice,
        "Command does not accept arguments");
    const CommandResult idle_stop_with_argument =
        handle_text_input(*controller, "operator", "/stop later");
    EXPECT_TRUE(idle_stop_with_argument.clear_input);
    EXPECT_EQ(
        idle_stop_with_argument.session.notice,
        "Command does not accept arguments");

    const CommandResult unknown =
        handle_text_input(*controller, "operator", "/unknown");
    EXPECT_TRUE(unknown.clear_input);
    ASSERT_TRUE(unknown.session.notice);
    EXPECT_NE(unknown.session.notice->find("Unknown command"), std::string::npos);
    EXPECT_NE(unknown.session.notice->find("/mcast"), std::string::npos);

    const CommandResult empty_multicast =
        handle_text_input(*controller, "operator", "/mcast");
    EXPECT_TRUE(empty_multicast.clear_input);
    EXPECT_EQ(empty_multicast.session.notice, "Multicast prompt is empty");

    EXPECT_EQ(
        handle_text_input(*controller, "not-a-persona", "/clear").session.notice,
        "Transcript cleared");
    EXPECT_TRUE(has_state_update(handle_text_input(*controller, "operator", "/hide-on").session));
    EXPECT_TRUE(has_state_update(handle_text_input(*controller, "operator", "/hide").session));
    EXPECT_TRUE(has_state_update(handle_text_input(*controller, "operator", "/hide-off").session));
    EXPECT_EQ(
        copy_entries(controller->view().transcript),
        (std::vector<TranscriptEntry>{
            make_hide_on_marker(1),
            make_hide_marker(2),
            make_hide_off_marker(3),
        }));
    const CommandResult information =
        handle_text_input(*controller, "operator", "/info");
    ASSERT_TRUE(information.session.notice);
    EXPECT_NE(
        information.session.notice->find("Transcript entries: 3"),
        std::string::npos);
    const CommandResult characters =
        handle_text_input(*controller, "operator", "/characters");
    ASSERT_TRUE(characters.session.notice);
    EXPECT_NE(characters.session.notice->find("@Guide"), std::string::npos);
    EXPECT_EQ(
        handle_text_input(*controller, "operator", "/@Gui").session.notice,
        "Default character is now Guide");

    const CommandResult idle_stop =
        handle_text_input(*controller, "operator", "/stop");
    EXPECT_TRUE(idle_stop.clear_input);
    EXPECT_EQ(idle_stop.session.notice, "No generation is active");

    const CommandResult exit =
        handle_text_input(*controller, "operator", "/exit");
    EXPECT_TRUE(exit.clear_input);
    EXPECT_TRUE(exit.close_session);
}

TEST(TextInput, ParsesAnAddressedPromptBeforeSubmission) {
    TemporaryTextSession temporary;
    auto controller = test::from_definitions_for_testing(
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

TEST(TextInput, ForwardsAuthorOnlyToBatchStartingCommands) {
    TemporaryTextSession ordinary_temporary;
    auto ordinary_controller = test::from_backends_for_testing(
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
    auto multicast_controller = test::from_backends_for_testing(
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
    auto controller = test::from_definitions_for_testing(
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

TEST(TextInput, PreservesDraftsAndAcceptsStopDuringGeneration) {
    TemporaryTextSession temporary;
    auto controller = test::from_backends_for_testing(
        test::one_backend(std::make_unique<BlockingBackend>()),
        temporary.path,
        notifier());

    (void)handle_text_input(*controller, "operator", "Question");
    const CommandResult blocked =
        handle_text_input(*controller, "operator", "Another");
    EXPECT_FALSE(blocked.clear_input);
    EXPECT_EQ(
        blocked.session.notice,
        "Generation in progress; use /stop, Esc, or Ctrl-C");

    const CommandResult hidden_while_active =
        handle_text_input(*controller, "operator", "/hide");
    EXPECT_FALSE(hidden_while_active.clear_input);
    EXPECT_EQ(
        hidden_while_active.session.notice,
        "Generation in progress; use /stop, Esc, or Ctrl-C");

    const CommandResult stop_with_argument =
        handle_text_input(*controller, "operator", "/stop later");
    EXPECT_FALSE(stop_with_argument.clear_input);
    EXPECT_EQ(
        stop_with_argument.session.notice,
        "Generation in progress; use /stop, Esc, or Ctrl-C");

    const CommandResult stopping =
        handle_text_input(*controller, "operator", "/stop");
    EXPECT_TRUE(stopping.clear_input);
    EXPECT_EQ(stopping.session.notice, "Stopping generation...");
    controller->shutdown();
}

TEST(TextInput, SeparatesDraftClearingFromControllerAcceptanceAndExit) {
    TemporaryTextSession temporary;
    auto controller = test::from_definitions_for_testing(
        std::vector<CharacterDefinition>{definition()}, temporary.path, notifier());

    const CommandResult unknown_author =
        handle_text_input(*controller, "unknown", "Question");
    EXPECT_FALSE(unknown_author.session.input_consumed);
    EXPECT_FALSE(unknown_author.clear_input);

    const CommandResult empty_default =
        handle_text_input(*controller, "operator", "/@");
    EXPECT_TRUE(empty_default.session.input_consumed);
    EXPECT_TRUE(empty_default.clear_input);

    const CommandResult unresolved_default =
        handle_text_input(*controller, "operator", "/@Nobody");
    EXPECT_TRUE(unresolved_default.session.input_consumed);
    EXPECT_TRUE(unresolved_default.clear_input);

    const CommandResult parse_error =
        handle_text_input(*controller, "operator", "/clear later");
    EXPECT_FALSE(parse_error.session.input_consumed);
    EXPECT_TRUE(parse_error.clear_input);

    // A recognized command that fails its precondition still consumes the line
    // it was typed on. Only composed prompt text survives a rejection.
    const CommandResult no_span =
        handle_text_input(*controller, "operator", "/hide");
    EXPECT_TRUE(no_span.session.input_consumed);
    EXPECT_TRUE(no_span.clear_input);
    EXPECT_FALSE(has_state_update(no_span.session));

    const CommandResult nothing_to_restore =
        handle_text_input(*controller, "operator", "/hide-off");
    EXPECT_TRUE(nothing_to_restore.session.input_consumed);
    EXPECT_TRUE(nothing_to_restore.clear_input);
    EXPECT_FALSE(has_state_update(nothing_to_restore.session));

    const CommandResult exit_result =
        handle_text_input(*controller, "operator", "/exit");
    EXPECT_TRUE(exit_result.clear_input);
    EXPECT_TRUE(exit_result.close_session);
    EXPECT_FALSE(exit_result.session.session_ended);

    controller->shutdown();
    const CommandResult undispatchable =
        handle_text_input(*controller, "operator", "Another question");
    EXPECT_FALSE(undispatchable.session.input_consumed);
    EXPECT_FALSE(undispatchable.clear_input);
}

} // namespace
} // namespace cha::web
