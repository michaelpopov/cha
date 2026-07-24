#include "agents/agent_context.h"
#include "conversation/conversation.h"
#include "storage/session_database.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace cha {
namespace {

// Runs one statement directly against a session database and returns its SQLite code.
// The tests use it to reach schema constraints that the journal API refuses to violate.
int raw_execute(const std::filesystem::path& path, const std::string& sql) {
    sqlite3* handle = nullptr;
    if (sqlite3_open_v2(path.c_str(), &handle, SQLITE_OPEN_READWRITE, nullptr)
        != SQLITE_OK) {
        sqlite3_close_v2(handle);
        throw std::runtime_error("Failed to open '" + path.string() + "' directly");
    }
    const int result = sqlite3_exec(handle, sql.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close_v2(handle);
    return result;
}

std::vector<std::string> table_columns(
    const std::filesystem::path& path,
    const std::string& table) {
    sqlite3* handle = nullptr;
    if (sqlite3_open_v2(path.c_str(), &handle, SQLITE_OPEN_READONLY, nullptr)
        != SQLITE_OK) {
        sqlite3_close_v2(handle);
        throw std::runtime_error("Failed to open '" + path.string() + "' directly");
    }
    sqlite3_stmt* statement = nullptr;
    const std::string sql = "PRAGMA table_info(" + table + ")";
    std::vector<std::string> columns;
    if (sqlite3_prepare_v2(handle, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK) {
        while (sqlite3_step(statement) == SQLITE_ROW) {
            columns.emplace_back(
                reinterpret_cast<const char*>(sqlite3_column_text(statement, 1)));
        }
    }
    sqlite3_finalize(statement);
    sqlite3_close_v2(handle);
    return columns;
}

std::filesystem::path temporary_path(std::string_view prefix) {
    return std::filesystem::temp_directory_path()
        / (std::string(prefix)
           + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())
           + ".sqlite3");
}

void create_test_database(const std::filesystem::path& path) {
    if (!create_session_database(
            path,
            {
                .id = path.stem().string(),
                .room = "test-room",
                .label = "Test session",
            })) {
        throw std::runtime_error("Failed to create test session database");
    }
}

ConversationEntry human(
    EntryId id,
    std::string text,
    std::optional<RequestId> request_id = std::nullopt) {
    return make_human_entry(
        id, "reviewer-id", "Reviewer", std::move(text), request_id);
}

TEST(Conversation, StoresTypedCompleteAndStreamingEntries) {
    Conversation conversation;
    conversation.add_entry(human(1, "Review this code", 10));
    conversation.begin_entry(make_agent_entry(
        2, "reviewer-id", "Reviewer", std::string{}, CompletionStatus::streaming, 10));
    conversation.append_to_entry(2, CompletionDeltaKind::answer, "Two ");
    conversation.append_to_entry(2, CompletionDeltaKind::answer, "issues found");

    EXPECT_EQ(conversation.open_entry_id(), 2U);
    conversation.finish_entry(2, CompletionStatus::complete);
    EXPECT_FALSE(conversation.open_entry_id());
    EXPECT_EQ(
        conversation.entries(),
        (std::vector<ConversationEntry>{
            human(1, "Review this code", 10),
            make_agent_entry(
                2, "reviewer-id", "Reviewer", "Two issues found", CompletionStatus::complete, 10),
        }));
}

TEST(Conversation, RequiresTheStreamingEntryHandleForMutation) {
    Conversation conversation;
    conversation.begin_entry(make_agent_entry(
        4, "reviewer-id", "Reviewer", std::string{}, CompletionStatus::streaming, 2));

    EXPECT_THROW(
        conversation.append_to_entry(
            5, CompletionDeltaKind::answer, "wrong"),
        std::logic_error);
    EXPECT_THROW(conversation.discard_entry(5), std::logic_error);
    EXPECT_EQ(conversation.open_entry_id(), 4U);
}

TEST(Conversation, SeparatesReasoningFromAnswerAndValidatesTerminalContent) {
    Conversation conversation;
    conversation.begin_entry(make_agent_entry(
        1,
        "reviewer-id",
        "Reviewer",
        std::string{},
        CompletionStatus::streaming,
        1));
    conversation.append_to_entry(
        1, CompletionDeltaKind::reasoning, "PRIVATE_REASONING");
    EXPECT_EQ(conversation.entries().back().reasoning_text, "PRIVATE_REASONING");
    EXPECT_TRUE(conversation.entries().back().text.empty());
    EXPECT_THROW(
        conversation.finish_entry(1, CompletionStatus::complete),
        std::invalid_argument);
    EXPECT_NO_THROW(
        conversation.finish_entry(1, CompletionStatus::cancelled));

    ConversationEntry non_agent = make_notice_entry(2, "Notice");
    non_agent.reasoning_text = "invalid";
    EXPECT_THROW(
        validate_conversation_entry(non_agent),
        std::invalid_argument);
    EXPECT_THROW(
        require_storable_conversation_entry(conversation.entries().back()),
        std::invalid_argument);
}

TEST(ConversationJournal, RejectsReasoningAndRollsBackTheTurn) {
    const std::filesystem::path path =
        temporary_path("cha_reasoning_guard_");
    create_test_database(path);
    ConversationJournal journal(path);
    const ConversationEntry prompt =
        make_human_entry(1, "reviewer-id", "Reviewer", "Question", 1);
    journal.start_turn(1, prompt);
    const ConversationEntry response = make_agent_entry(
        2,
        "reviewer-id",
        "Reviewer",
        {
            .reasoning = "UNIQUE_REASONING_MARKER_42",
            .answer = "Answer",
        },
        CompletionStatus::complete,
        1);

    EXPECT_THROW(journal.complete_turn(1, response), std::runtime_error);
    const ConversationRestore restored = load_conversation_state(path);
    ASSERT_EQ(restored.entries.size(), 1U);
    EXPECT_EQ(restored.entries.front(), prompt);
    ASSERT_EQ(restored.interrupted_turns.size(), 1U);

    std::ifstream database(path, std::ios::binary);
    const std::string bytes{
        std::istreambuf_iterator<char>(database),
        std::istreambuf_iterator<char>()};
    EXPECT_EQ(bytes.find("UNIQUE_REASONING_MARKER_42"), std::string::npos);
    std::filesystem::remove(path);
}

TEST(Conversation, ReturnsAnIndependentEntrySnapshot) {
    Conversation conversation;
    conversation.add_entry(make_notice_entry(1, "Original"));

    auto snapshot = conversation.entries();
    snapshot.front().text = "Changed";

    EXPECT_EQ(conversation.entries().front().text, "Original");
}

TEST(Conversation, ReadViewIsLockedNonOwningAndNeitherCopyableNorMovable) {
    static_assert(!std::is_copy_constructible_v<ConversationReadView>);
    static_assert(!std::is_move_constructible_v<ConversationReadView>);

    Conversation conversation;
    conversation.add_entry(make_notice_entry(1, "Original"));
    ConversationReadView view = conversation.read();

    EXPECT_EQ(view.entries().size(), 1U);
    EXPECT_EQ(view.entries().front().text, "Original");
    EXPECT_FALSE(view.open_entry_id());
}

TEST(Conversation, MutationWaitsUntilTheReadViewIsDestroyed) {
    Conversation conversation;
    conversation.add_entry(make_notice_entry(1, "Original"));
    std::promise<void> view_ready;
    std::future<void> ready = view_ready.get_future();
    std::promise<void> release_view;
    std::shared_future<void> release = release_view.get_future().share();

    std::thread reader([&] {
        ConversationReadView view = conversation.read();
        view_ready.set_value();
        release.wait();
    });
    ASSERT_EQ(ready.wait_for(std::chrono::seconds(1)), std::future_status::ready);

    std::future<void> mutation = std::async(std::launch::async, [&] {
        conversation.add_entry(make_notice_entry(2, "Later"));
    });
    EXPECT_EQ(mutation.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);

    release_view.set_value();
    reader.join();
    EXPECT_EQ(mutation.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    mutation.get();
}

TEST(Conversation, ReplacesAndClearsEntries) {
    Conversation conversation;
    conversation.add_entry(make_notice_entry(1, "Old"));
    const std::size_t initial_epoch = conversation.snapshot().history_epoch;
    conversation.replace_entries({
        human(2, "Restored"),
        make_agent_entry(3, "guide-id", "Guide", "Welcome", CompletionStatus::complete),
    });

    EXPECT_EQ(conversation.entries().size(), 2U);
    EXPECT_EQ(conversation.snapshot().history_epoch, initial_epoch + 1);
    conversation.clear();
    EXPECT_TRUE(conversation.entries().empty());
    EXPECT_EQ(conversation.snapshot().history_epoch, initial_epoch + 2);
}

TEST(Conversation, RequiresStrictlyIncreasingEntryIds) {
    Conversation conversation;
    conversation.add_entry(make_notice_entry(2, "First"));

    EXPECT_THROW(conversation.add_entry(make_notice_entry(2, "Duplicate")), std::invalid_argument);
    EXPECT_THROW(conversation.add_entry(make_notice_entry(1, "Out of order")), std::invalid_argument);
    EXPECT_NO_THROW(conversation.add_entry(make_notice_entry(5, "Gap is allowed")));

    EXPECT_THROW(
        conversation.replace_entries({
            make_notice_entry(10, "Later"),
            make_notice_entry(9, "Earlier"),
        }),
        std::invalid_argument);
}

TEST(ConversationValidation, IsEnforcedByMemoryAndDatabase) {
    ConversationEntry invalid = make_error_entry(1, "Failure");
    invalid.status = CompletionStatus::complete;
    EXPECT_THROW(validate_conversation_entry(invalid), std::invalid_argument);

    Conversation conversation;
    EXPECT_THROW(conversation.add_entry(invalid), std::invalid_argument);

    const auto path = temporary_path("cha_invalid_entry_");
    create_test_database(path);
    ConversationJournal journal(path);
    EXPECT_THROW(journal.append(invalid), std::runtime_error);

    const ConversationEntry empty_completion = make_agent_entry(
        2, "reviewer-id", "Reviewer", std::string{}, CompletionStatus::complete, 1);
    EXPECT_THROW(validate_conversation_entry(empty_completion), std::invalid_argument);
    EXPECT_THROW(conversation.add_entry(empty_completion), std::invalid_argument);
    EXPECT_THROW(journal.append(empty_completion), std::runtime_error);

    Conversation streaming;
    streaming.begin_entry(make_agent_entry(
        1, "reviewer-id", "Reviewer", std::string{}, CompletionStatus::streaming, 1));
    EXPECT_THROW(
        streaming.finish_entry(1, CompletionStatus::complete),
        std::invalid_argument);
    std::filesystem::remove(path);
}

TEST(SessionDatabase, RoundTripsMetadataAndTypedEntries) {
    const auto path = temporary_path("cha_conversation_");
    create_test_database(path);
    ConversationJournal journal(path);
    journal.start_turn(1, human(1, "Hello", 1));
    journal.complete_turn(1, make_agent_entry(
        2, "reviewer-id", "Reviewer", "Hello back", CompletionStatus::complete, 1));
    journal.append(make_notice_entry(3, "Information"));

    EXPECT_EQ(
        load_conversation_entries(path),
        (std::vector<ConversationEntry>{
            human(1, "Hello", 1),
            make_agent_entry(
                2,
                "reviewer-id",
                "Reviewer",
                "Hello back",
                CompletionStatus::complete,
                1),
            make_notice_entry(3, "Information"),
        }));
    const SessionDatabaseMetadata metadata =
        read_session_database_metadata(path);
    EXPECT_EQ(metadata.id, path.stem().string());
    EXPECT_EQ(metadata.label, "Test session");
    std::filesystem::remove(path);
}

TEST(SessionDatabase, RejectsAStreamingEntry) {
    const auto path = temporary_path("cha_open_conversation_");
    create_test_database(path);
    ConversationJournal journal(path);
    EXPECT_THROW(
        journal.append(make_agent_entry(
            1,
            "reviewer-id",
            "Reviewer",
            std::string{},
            CompletionStatus::streaming,
            1)),
        std::runtime_error);
    EXPECT_TRUE(load_conversation_entries(path).empty());
    std::filesystem::remove(path);
}

TEST(ConversationJournal, ReplaysStandaloneEntriesAndClearEvents) {
    const auto path = temporary_path("cha_journal_");
    create_test_database(path);
    ConversationJournal journal(path);
    journal.append(make_notice_entry(1, "Old"));
    journal.clear();
    journal.append(make_notice_entry(2, "Current"));

    EXPECT_EQ(
        load_conversation_entries(path),
        (std::vector<ConversationEntry>{make_notice_entry(2, "Current")}));
    std::filesystem::remove(path);
}

TEST(ConversationJournal, RejectsOutOfOrderEntryIdsWithoutChangingStoredState) {
    const auto path = temporary_path("cha_out_of_order_journal_");
    create_test_database(path);
    ConversationJournal journal(path);
    journal.append(make_notice_entry(2, "Later ID"));

    EXPECT_THROW(
        journal.append(make_notice_entry(1, "Earlier ID")),
        std::invalid_argument);
    EXPECT_EQ(
        load_conversation_entries(path),
        (std::vector<ConversationEntry>{make_notice_entry(2, "Later ID")}));
    std::filesystem::remove(path);
}

TEST(ConversationJournal, RollsBackAnInvalidTerminalTransition) {
    const auto path = temporary_path("cha_rollback_journal_");
    create_test_database(path);
    ConversationJournal journal(path);
    journal.start_turn(1, human(1, "Question", 1));
    EXPECT_THROW(
        journal.complete_turn(
            1,
            make_agent_entry(
                1,
                "guide-id",
                "Guide",
                "Answer",
                CompletionStatus::complete,
                1)),
        std::invalid_argument);

    const ConversationRestore restored = load_conversation_state(path);
    ASSERT_EQ(restored.interrupted_turns.size(), 1U);
    EXPECT_EQ(restored.entries.front(), human(1, "Question", 1));
    std::filesystem::remove(path);
}

TEST(ConversationJournal, ReplaysIdentifiedTypedTurnOutcomes) {
    const auto path = temporary_path("cha_identified_journal_");
    create_test_database(path);
    ConversationJournal journal(path);
    journal.start_turn(7, human(1, "First", 7));
    journal.complete_turn(7, make_agent_entry(
        2, "guide-id", "Guide", "Answer", CompletionStatus::complete, 7));
    journal.start_turn(8, human(3, "Second", 8));
    journal.fail_turn(8, make_error_entry(4, "Unavailable", 8, "guide-id"));

    const ConversationRestore restored = load_conversation_state(path);
    EXPECT_EQ(restored.next_request_id, 9U);
    EXPECT_EQ(restored.next_entry_id, 5U);
    EXPECT_TRUE(restored.interrupted_turns.empty());
    EXPECT_EQ(
        restored.entries,
        (std::vector<ConversationEntry>{
            human(1, "First", 7),
            make_agent_entry(2, "guide-id", "Guide", "Answer", CompletionStatus::complete, 7),
            human(3, "Second", 8),
            make_error_entry(4, "Unavailable", 8, "guide-id"),
        }));
    std::filesystem::remove(path);
}

TEST(ConversationJournal, RejectsEntriesThatDoNotMatchTheirTurnRecords) {
    const auto path = temporary_path("cha_invalid_turn_entry_");
    create_test_database(path);
    ConversationJournal journal(path);

    EXPECT_THROW(
        journal.start_turn(7, human(1, "Prompt", 8)),
        std::invalid_argument);
    EXPECT_THROW(
        journal.complete_turn(7, make_agent_entry(
            2, "guide-id", "Guide", "Answer", CompletionStatus::cancelled, 7)),
        std::invalid_argument);
    EXPECT_THROW(
        journal.cancel_turn(7, make_agent_entry(
            2, "guide-id", "Guide", "Answer", CompletionStatus::complete, 7)),
        std::invalid_argument);
    EXPECT_THROW(
        journal.fail_turn(7, make_error_entry(2, "Failure", 8, "guide-id")),
        std::invalid_argument);

    std::filesystem::remove(path);
}

TEST(ConversationJournal, RecognizesAnInterruptedTypedTurn) {
    const auto path = temporary_path("cha_interrupted_journal_");
    create_test_database(path);
    ConversationJournal journal(path);
    const ConversationEntry prompt =
        make_human_entry(5, "guide-id", "Guide", "Pending", 12);
    journal.start_turn(12, prompt);

    const ConversationRestore restored = load_conversation_state(path);
    ASSERT_EQ(restored.interrupted_turns.size(), 1U);
    EXPECT_EQ(
        restored.entries,
        (std::vector<ConversationEntry>{prompt}));
    EXPECT_EQ(
        load_conversation_entries(path),
        (std::vector<ConversationEntry>{prompt}));
    EXPECT_EQ(restored.interrupted_turns.front().request_id, 12U);
    EXPECT_EQ(restored.interrupted_turns.front().error_entry.kind, EntryKind::error);
    EXPECT_EQ(restored.interrupted_turns.front().error_entry.participant_id, "guide-id");
    EXPECT_EQ(restored.next_request_id, 13U);
    EXPECT_EQ(restored.next_entry_id, 7U);
    std::filesystem::remove(path);
}

TEST(SessionDatabase, RejectsANonDatabaseFile) {
    const auto path = temporary_path("cha_old_journal_");
    {
        std::ofstream file(path, std::ios::binary);
        file << R"({"type":"conversation","version":4})" << '\n';
    }

    EXPECT_THROW(ConversationJournal journal(path), std::runtime_error);
    EXPECT_THROW((void)load_conversation_state(path), std::runtime_error);
    std::filesystem::remove(path);
}

TEST(SessionDatabase, JournalDoesNotCreateAMissingDatabase) {
    const auto path = temporary_path("cha_missing_database_");

    EXPECT_THROW(ConversationJournal journal(path), std::runtime_error);
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(ConversationValidation, RequiresATargetOnHumanEntriesAndForbidsItElsewhere) {
    ConversationEntry untargeted = human(1, "No target");
    untargeted.addressed_to.clear();
    EXPECT_THROW(validate_conversation_entry(untargeted), std::invalid_argument);

    ConversationEntry unnamed = human(1, "No target name");
    unnamed.addressed_to_name.clear();
    EXPECT_THROW(validate_conversation_entry(unnamed), std::invalid_argument);

    ConversationEntry addressed_agent = make_agent_entry(
        2, "guide-id", "Guide", "Answer", CompletionStatus::complete);
    addressed_agent.addressed_to = "reviewer-id";
    addressed_agent.addressed_to_name = "Reviewer";
    EXPECT_THROW(validate_conversation_entry(addressed_agent), std::invalid_argument);

    ConversationEntry addressed_notice = make_notice_entry(3, "Notice");
    addressed_notice.addressed_to_name = "Reviewer";
    EXPECT_THROW(validate_conversation_entry(addressed_notice), std::invalid_argument);

    ConversationEntry addressed_error = make_error_entry(4, "Failure");
    addressed_error.addressed_to = "reviewer-id";
    EXPECT_THROW(validate_conversation_entry(addressed_error), std::invalid_argument);

    EXPECT_NO_THROW(validate_conversation_entry(human(1, "Targeted")));
}

TEST(ConversationValidation, RejectsAddressingViolationsInMemoryAndInSqlite) {
    ConversationEntry untargeted = human(1, "No target");
    untargeted.addressed_to.clear();
    untargeted.addressed_to_name.clear();

    Conversation conversation;
    EXPECT_THROW(conversation.add_entry(untargeted), std::invalid_argument);

    const auto path = temporary_path("cha_addressing_");
    create_test_database(path);
    ConversationJournal journal(path);
    EXPECT_THROW(journal.append(untargeted), std::runtime_error);
    EXPECT_TRUE(load_conversation_entries(path).empty());
    std::filesystem::remove(path);
}

TEST(SessionDatabase, RoundTripsTheAddressedTargetOfEveryPrompt) {
    const auto path = temporary_path("cha_addressed_round_trip_");
    create_test_database(path);
    ConversationJournal journal(path);
    journal.start_turn(1, make_human_entry(1, "ismael", "Ismael", "And you?", 1));
    journal.complete_turn(1, make_agent_entry(
        2, "ismael", "Ismael", "Call me Ismael.", CompletionStatus::complete, 1));

    const std::vector<ConversationEntry> restored = load_conversation_entries(path);
    ASSERT_EQ(restored.size(), 2U);
    EXPECT_EQ(restored.front().addressed_to, "ismael");
    EXPECT_EQ(restored.front().addressed_to_name, "Ismael");
    EXPECT_TRUE(restored.back().addressed_to.empty());
    EXPECT_TRUE(restored.back().addressed_to_name.empty());
    std::filesystem::remove(path);
}

TEST(SessionDatabase, RefusesAVersionOneDatabase) {
    const auto path = temporary_path("cha_version_one_");
    create_test_database(path);
    ASSERT_EQ(raw_execute(path, "PRAGMA user_version = 1"), SQLITE_OK);

    try {
        (void)load_conversation_state(path);
        FAIL() << "expected the older schema version to be refused";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("Unsupported session database"),
                  std::string::npos)
            << error.what();
    }
    EXPECT_THROW(ConversationJournal journal(path), std::runtime_error);
    EXPECT_THROW((void)read_session_database_metadata(path), std::runtime_error);
    std::filesystem::remove(path);
}

TEST(SessionDatabase, StoresTheTargetOnlyOnThePromptItself) {
    const auto path = temporary_path("cha_schema_shape_");
    create_test_database(path);

    EXPECT_EQ(
        table_columns(path, "turns"),
        (std::vector<std::string>{"request_id", "epoch", "state"}))
        << "the turn must not duplicate its prompt's target";

    const std::vector<std::string> entries = table_columns(path, "entries");
    EXPECT_NE(std::find(entries.begin(), entries.end(), "addressed_to"), entries.end());
    EXPECT_NE(std::find(entries.begin(), entries.end(), "addressed_to_name"), entries.end());

    EXPECT_EQ(
        table_columns(path, "session"),
        (std::vector<std::string>{"singleton", "id", "room", "label"}))
        << "a session belongs to a room, not to a roster";
    std::filesystem::remove(path);
}

TEST(SessionDatabase, ConstrainsAddressingColumnsByEntryKind) {
    const auto path = temporary_path("cha_addressing_checks_");
    create_test_database(path);
    const auto insert = [&path](
        int entry_id, int kind, std::string_view participant, std::string_view name,
        std::string_view addressed_to, std::string_view addressed_to_name) {
        return raw_execute(
            path,
            "INSERT INTO entries (entry_id, epoch, request_id, kind, participant_id,"
            " display_name, addressed_to, addressed_to_name, text, status) VALUES ("
                + std::to_string(entry_id) + ", 1, NULL, " + std::to_string(kind)
                + ", '" + std::string(participant) + "', '" + std::string(name)
                + "', '" + std::string(addressed_to) + "', '"
                + std::string(addressed_to_name) + "', 'Text', 0)");
    };

    EXPECT_EQ(insert(1, 0, "human", "You", "ismael", "Ismael"), SQLITE_OK);
    EXPECT_EQ(insert(2, 0, "human", "You", "", ""), SQLITE_CONSTRAINT);
    EXPECT_EQ(insert(3, 0, "human", "You", "ismael", ""), SQLITE_CONSTRAINT);
    EXPECT_EQ(insert(4, 0, "human", "You", "", "Ismael"), SQLITE_CONSTRAINT);
    EXPECT_EQ(insert(5, 1, "ismael", "Ismael", "human", "You"), SQLITE_CONSTRAINT);
    EXPECT_EQ(insert(6, 2, "", "System", "ismael", "Ismael"), SQLITE_CONSTRAINT);
    EXPECT_EQ(insert(7, 2, "", "System", "", "Ismael"), SQLITE_CONSTRAINT);
    EXPECT_EQ(insert(8, 1, "ismael", "Ismael", "", ""), SQLITE_OK);
    std::filesystem::remove(path);
}

TEST(SessionDatabase, AllowsOnlyOnePromptPerTurn) {
    const auto path = temporary_path("cha_one_prompt_");
    create_test_database(path);
    ASSERT_EQ(
        raw_execute(path, "INSERT INTO turns (request_id, epoch, state) VALUES (1, 1, 1)"),
        SQLITE_OK);
    const auto insert_entry = [&path](int entry_id, int kind, std::string_view participant) {
        return raw_execute(
            path,
            "INSERT INTO entries (entry_id, epoch, request_id, kind, participant_id,"
            " display_name, addressed_to, addressed_to_name, text, status) VALUES ("
                + std::to_string(entry_id) + ", 1, 1, " + std::to_string(kind) + ", '"
                + std::string(participant) + "', 'Name', "
                + (kind == 0 ? "'ismael', 'Ismael'" : "'', ''") + ", 'Text', 0)");
    };

    EXPECT_EQ(insert_entry(1, 0, "human"), SQLITE_OK);
    EXPECT_EQ(insert_entry(2, 0, "human"), SQLITE_CONSTRAINT);
    EXPECT_EQ(insert_entry(3, 1, "ismael"), SQLITE_OK)
        << "the index constrains prompts only, not the turn's response";
    std::filesystem::remove(path);
}

TEST(SessionDatabase, RejectsATurnThatHasNoPrompt) {
    const auto path = temporary_path("cha_turn_without_prompt_");
    create_test_database(path);
    ASSERT_EQ(
        raw_execute(path, "INSERT INTO turns (request_id, epoch, state) VALUES (4, 1, 1)"),
        SQLITE_OK);

    try {
        (void)load_conversation_state(path);
        FAIL() << "expected a turn without a prompt to be rejected";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("turn without exactly one prompt"),
                  std::string::npos)
            << error.what();
    }
    EXPECT_THROW(ConversationJournal journal(path), std::runtime_error);
    std::filesystem::remove(path);
}

TEST(SessionDatabase, RecoversAnInterruptedTurnFromItsPersistedPrompt) {
    const auto path = temporary_path("cha_interrupted_target_");
    create_test_database(path);
    {
        ConversationJournal journal(path);
        journal.start_turn(1, make_human_entry(1, "cheburashka", "Cheburashka", "Who are you?", 1));
        journal.complete_turn(1, make_agent_entry(
            2, "cheburashka", "Cheburashka", "I am Cheburashka.",
            CompletionStatus::complete, 1));
        journal.start_turn(2, make_human_entry(3, "ismael", "Ismael", "And you?", 2));
    }

    const ConversationRestore restored = load_conversation_state(path);

    ASSERT_EQ(restored.interrupted_turns.size(), 1U);
    const InterruptedTurn& interrupted = restored.interrupted_turns.front();
    EXPECT_EQ(interrupted.request_id, 2U);
    EXPECT_EQ(interrupted.error_entry.participant_id, "ismael")
        << "the error belongs to the agent the prompt was addressed to";
    EXPECT_EQ(interrupted.error_entry.request_id, 2U);
    EXPECT_EQ(restored.entries.size(), 3U);
    std::filesystem::remove(path);
}

TEST(SessionDatabase, RestoresAndProjectsASessionWhoseRoomLostAnAgent) {
    const auto path = temporary_path("cha_roster_drift_");
    create_test_database(path);
    {
        ConversationJournal journal(path);
        journal.start_turn(1, make_human_entry(1, "cheburashka", "Cheburashka", "Who are you?", 1));
        journal.complete_turn(1, make_agent_entry(
            2, "cheburashka", "Cheburashka", "I am Cheburashka.",
            CompletionStatus::complete, 1));
        journal.start_turn(2, make_human_entry(3, "ismael", "Ismael", "And you?", 2));
        journal.complete_turn(2, make_agent_entry(
            4, "ismael", "Ismael", "Call me Ismael.", CompletionStatus::complete, 2));
    }

    // Cheburashka has left the room; only Ismael remains on the roster.
    const ConversationRestore restored = load_conversation_state(path);

    EXPECT_TRUE(restored.interrupted_turns.empty());
    ASSERT_EQ(restored.entries.size(), 4U);
    EXPECT_EQ(restored.entries.front().addressed_to, "cheburashka");
    EXPECT_EQ(restored.entries[1].display_name, "Cheburashka");

    ConversationSnapshot snapshot{.entries = restored.entries};
    EXPECT_EQ(
        project_agent_context(
            snapshot.entries,
            snapshot.open_entry_id,
            "Ismael system",
            "ismael"),
        (std::vector<AgentMessage>{
            {AgentRole::system, "Ismael system"},
            {AgentRole::user,
             "User: [to Cheburashka] Who are you?"
             "\n\nCheburashka: I am Cheburashka."
             "\n\nUser: And you?"},
            {AgentRole::assistant, "Call me Ismael."},
        }));
    std::filesystem::remove(path);
}

} // namespace
} // namespace cha
