#include "agents/model_context.h"
#include "chat/transcript.h"
#include "session/session_database.h"
#include "support/test_session_database.h"
#include "support/test_transcript.h"
#include "util/path_name.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cha {
namespace {

void expect_same_entry(TranscriptEntry actual, TranscriptEntry expected) {
    // Factory-rebuilt expected values can straddle a second boundary.
    actual.created_at = 0;
    expected.created_at = 0;
    EXPECT_EQ(actual, expected);
}

void expect_entries(
    std::span<const TranscriptEntry> actual,
    const std::vector<TranscriptEntry>& expected) {
    ASSERT_EQ(actual.size(), expected.size());
    for (std::size_t index = 0; index < actual.size(); ++index) {
        expect_same_entry(actual[index], expected[index]);
    }
}

// Runs one statement directly against a session database and returns its SQLite code.
// The tests use it to reach schema constraints that the journal API refuses to violate.
int raw_execute(const std::filesystem::path& path, const std::string& sql) {
    sqlite3* handle = nullptr;
    const std::string database_path = utf8_path(path);
    if (sqlite3_open_v2(
            database_path.c_str(),
            &handle,
            SQLITE_OPEN_READWRITE,
            nullptr)
        != SQLITE_OK) {
        sqlite3_close_v2(handle);
        throw std::runtime_error(
            "Failed to open '" + database_path + "' directly");
    }
    const int result = sqlite3_exec(handle, sql.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close_v2(handle);
    return result;
}

std::vector<std::string> table_columns(
    const std::filesystem::path& path,
    const std::string& table) {
    sqlite3* handle = nullptr;
    const std::string database_path = utf8_path(path);
    if (sqlite3_open_v2(
            database_path.c_str(),
            &handle,
            SQLITE_OPEN_READONLY,
            nullptr)
        != SQLITE_OK) {
        sqlite3_close_v2(handle);
        throw std::runtime_error(
            "Failed to open '" + database_path + "' directly");
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
                .id = utf8_path(path.stem()),
                .forum = "test-forum",
                .label = "Test session",
            })) {
        throw std::runtime_error("Failed to create test session database");
    }
}

TranscriptEntry human(
    EntryId id,
    std::string text,
    std::optional<RequestId> request_id = std::nullopt) {
    return test::human_entry(
        id, {"human", "You"}, {"reviewer-id", "Reviewer"}, std::move(text), request_id);
}

TEST(Transcript, StoresTypedCompleteAndStreamingEntries) {
    Transcript transcript;
    transcript.add_entry(human(1, "Review this code", 10));
    transcript.begin_entry(make_character_entry(
        2, "reviewer-id", "Reviewer", std::string{}, EntryStatus::streaming, 10));
    transcript.append_answer(2, "Two ");
    transcript.append_answer(2, "issues found");

    EXPECT_EQ(transcript.view().open_entry_id, 2U);
    EXPECT_EQ(transcript.open_entry_text(2), "Two issues found");
    EXPECT_THROW(transcript.open_entry_text(3), std::logic_error);
    transcript.finish_entry(2, EntryStatus::complete);
    EXPECT_FALSE(transcript.view().open_entry_id);
    EXPECT_THROW(transcript.open_entry_text(2), std::logic_error);
    expect_entries(
        transcript.view().entries,
        (std::vector<TranscriptEntry>{
            human(1, "Review this code", 10),
            make_character_entry(
                2, "reviewer-id", "Reviewer", "Two issues found", EntryStatus::complete, 10),
        }));
}

TEST(Transcript, RequiresTheStreamingEntryHandleForMutation) {
    Transcript transcript;
    transcript.begin_entry(make_character_entry(
        4, "reviewer-id", "Reviewer", std::string{}, EntryStatus::streaming, 2));

    EXPECT_THROW(
        transcript.append_answer(5, "wrong"),
        std::logic_error);
    EXPECT_THROW(transcript.discard_entry(5), std::logic_error);
    EXPECT_EQ(transcript.view().open_entry_id, 4U);
}

TEST(Transcript, RequiresAnswerTextForTerminalCharacterEntries) {
    Transcript transcript;
    transcript.begin_entry(make_character_entry(
        1,
        "reviewer-id",
        "Reviewer",
        std::string{},
        EntryStatus::streaming,
        1));
    EXPECT_THROW(
        transcript.finish_entry(1, EntryStatus::complete),
        std::invalid_argument);
    EXPECT_THROW(
        transcript.finish_entry(1, EntryStatus::cancelled),
        std::invalid_argument);
    transcript.append_answer(1, "Partial answer");
    EXPECT_NO_THROW(transcript.finish_entry(1, EntryStatus::cancelled));
    EXPECT_NO_THROW(
        require_storable_transcript_entry(transcript.view().entries.back()));
}

TEST(Transcript, ReplacesAndClearsEntries) {
    Transcript transcript;
    transcript.add_entry(make_notice_entry(1, "Old"));
    const std::size_t initial_epoch = transcript.view().history_epoch;
    transcript.replace_entries({
        human(2, "Restored"),
        make_character_entry(3, "guide-id", "Guide", "Welcome", EntryStatus::complete),
    });

    EXPECT_EQ(transcript.view().entries.size(), 2U);
    EXPECT_EQ(transcript.view().history_epoch, initial_epoch + 1);
    transcript.clear();
    EXPECT_TRUE(transcript.view().entries.empty());
    EXPECT_EQ(transcript.view().history_epoch, initial_epoch + 2);
}

TEST(Transcript, ManagesOffrecordBoundsAndTransientMarkersAtomically) {
    Transcript transcript;

    EXPECT_THROW((void)transcript.open_offrecord(0), std::invalid_argument);
    EXPECT_TRUE(transcript.view().entries.empty());
    EXPECT_FALSE(transcript.extend_offrecord(1));
    EXPECT_FALSE(transcript.restore_offrecord(1));
    EXPECT_TRUE(transcript.open_offrecord(1));
    EXPECT_FALSE(transcript.open_offrecord(2));
    transcript.add_entry(human(2, "Hidden prompt", 1));
    transcript.add_entry(make_character_entry(
        3, "reviewer-id", "Reviewer", "Hidden answer", EntryStatus::complete, 1));
    EXPECT_TRUE(transcript.extend_offrecord(4));

    const OffrecordSpan closed_span = transcript.model_history().offrecord_span;
    EXPECT_EQ(closed_span, (OffrecordSpan{.begin = 1, .end = 4}));
    EXPECT_TRUE(closed_span.contains(1));
    EXPECT_TRUE(closed_span.contains(3));
    EXPECT_FALSE(closed_span.contains(4));
    expect_entries(
        transcript.view().entries,
        (std::vector<TranscriptEntry>{
            make_hide_on_marker(1),
            human(2, "Hidden prompt", 1),
            make_character_entry(
                3, "reviewer-id", "Reviewer", "Hidden answer", EntryStatus::complete, 1),
            make_hide_marker(4),
        }));

    EXPECT_TRUE(transcript.restore_offrecord(5));
    EXPECT_EQ(transcript.model_history().offrecord_span, OffrecordSpan{});
    expect_same_entry(transcript.view().entries.back(), make_hide_off_marker(5));
    transcript.clear();
    EXPECT_EQ(transcript.model_history().offrecord_span, OffrecordSpan{});
}

TEST(Transcript, OffrecordBoundariesUseEntryIdsAndEachMarkerChangesOneRevision) {
    Transcript transcript;
    transcript.add_entry(human(2, "Earlier", 1));
    const TranscriptView before = transcript.view();

    EXPECT_TRUE(transcript.open_offrecord(5));
    const TranscriptView opened = transcript.view();
    EXPECT_EQ(opened.revision, before.revision + 1);
    EXPECT_EQ(opened.history_epoch, before.history_epoch);
    EXPECT_EQ(
        transcript.model_history().offrecord_span,
        (OffrecordSpan{.begin = 3, .end = std::nullopt}));

    transcript.add_entry(human(6, "Hidden", 2));
    const TranscriptView before_extend = transcript.view();
    EXPECT_TRUE(transcript.extend_offrecord(9));
    const TranscriptView closed = transcript.view();
    EXPECT_EQ(closed.revision, before_extend.revision + 1);
    EXPECT_EQ(closed.history_epoch, before_extend.history_epoch);
    EXPECT_EQ(
        transcript.model_history().offrecord_span,
        (OffrecordSpan{.begin = 3, .end = 7}));

    EXPECT_TRUE(transcript.restore_offrecord(12));
    const TranscriptView restored = transcript.view();
    EXPECT_EQ(restored.revision, closed.revision + 1);
    EXPECT_EQ(restored.history_epoch, closed.history_epoch);
}

TEST(Transcript, ReplacingEntriesDropsTheOffrecordSpan) {
    Transcript transcript;
    EXPECT_TRUE(transcript.open_offrecord(1));
    transcript.add_entry(human(2, "Hidden", 1));
    EXPECT_TRUE(transcript.extend_offrecord(3));
    ASSERT_NE(transcript.model_history().offrecord_span, OffrecordSpan{});

    transcript.replace_entries({human(20, "Restored", 2)});

    EXPECT_EQ(transcript.model_history().offrecord_span, OffrecordSpan{});
    expect_entries(
        transcript.view().entries,
        (std::vector<TranscriptEntry>{human(20, "Restored", 2)}));
}

TEST(Transcript, ModelHistoryOwnsOneAtomicModelContextSnapshot) {
    Transcript transcript;
    transcript.add_entry(human(1, "Question", 1));
    transcript.begin_entry(make_character_entry(
        2, "reviewer-id", "Reviewer", {}, EntryStatus::streaming, 1));
    const ModelHistory history = transcript.model_history();

    transcript.append_answer(2, "Live mutation");

    EXPECT_EQ(history.open_entry_id, 2U);
    ASSERT_EQ(history.entries.size(), 2U);
    EXPECT_TRUE(history.entries.back().text.empty());
    EXPECT_EQ(history.offrecord_span, OffrecordSpan{});
}

TEST(Transcript, ModelHistoryIncludesOffrecordProjectionState) {
    Transcript transcript;
    EXPECT_TRUE(transcript.open_offrecord(1));
    transcript.add_entry(human(2, "Hidden", 2));
    EXPECT_TRUE(transcript.extend_offrecord(3));

    const ModelHistory history = transcript.model_history();

    EXPECT_EQ(
        history.offrecord_span,
        (OffrecordSpan{.begin = 1, .end = 3}));
    expect_entries(transcript.view().entries, history.entries);
}

TEST(Transcript, RequiresStrictlyIncreasingEntryIds) {
    Transcript transcript;
    transcript.add_entry(make_notice_entry(2, "First"));

    EXPECT_THROW(transcript.add_entry(make_notice_entry(2, "Duplicate")), std::invalid_argument);
    EXPECT_THROW(transcript.add_entry(make_notice_entry(1, "Out of order")), std::invalid_argument);
    EXPECT_NO_THROW(transcript.add_entry(make_notice_entry(5, "Gap is allowed")));

    EXPECT_THROW(
        transcript.replace_entries({
            make_notice_entry(10, "Later"),
            make_notice_entry(9, "Earlier"),
        }),
        std::invalid_argument);
}

TEST(TranscriptValidation, IsEnforcedByMemoryAndDatabase) {
    TranscriptEntry invalid =
        make_error_entry(2, "Failure", 1, "reviewer-id");
    invalid.status = EntryStatus::complete;
    EXPECT_THROW(validate_transcript_entry(invalid), std::invalid_argument);

    Transcript transcript;
    EXPECT_THROW(transcript.add_entry(invalid), std::invalid_argument);

    const auto path = temporary_path("cha_invalid_entry_");
    create_test_database(path);
    auto journal = std::make_unique<SessionJournal>(path);
    const TranscriptEntry prompt = human(1, "Question", 1);
    journal->start_turn(1, prompt);
    EXPECT_THROW(journal->fail_turn(1, invalid), std::invalid_argument);

    const TranscriptEntry empty_response = make_character_entry(
        2, "reviewer-id", "Reviewer", std::string{}, EntryStatus::complete, 1);
    EXPECT_THROW(validate_transcript_entry(empty_response), std::invalid_argument);
    EXPECT_THROW(transcript.add_entry(empty_response), std::invalid_argument);
    EXPECT_THROW(
        journal->complete_turn(1, empty_response),
        std::runtime_error);
    EXPECT_EQ(
        load_transcript_entries(path),
        (std::vector<TranscriptEntry>{prompt}));

    Transcript streaming;
    streaming.begin_entry(make_character_entry(
        1, "reviewer-id", "Reviewer", std::string{}, EntryStatus::streaming, 1));
    EXPECT_THROW(
        streaming.finish_entry(1, EntryStatus::complete),
        std::invalid_argument);
    journal.reset();
    std::filesystem::remove(path);
}

TEST(SessionDatabase, RoundTripsMetadataAndTypedEntries) {
    const auto path = temporary_path("cha_transcript_");
    create_test_database(path);
    auto journal = std::make_unique<SessionJournal>(path);
    journal->start_turn(1, human(1, "Hello", 1));
    journal->complete_turn(1, make_character_entry(
        2, "reviewer-id", "Reviewer", "Hello back", EntryStatus::complete, 1));

    expect_entries(
        load_transcript_entries(path),
        {
            human(1, "Hello", 1),
            make_character_entry(
                2,
                "reviewer-id",
                "Reviewer",
                "Hello back",
                EntryStatus::complete,
                1),
        });
    const SessionDatabaseMetadata metadata =
        read_session_database_metadata(path);
    EXPECT_EQ(metadata.id, utf8_path(path.stem()));
    EXPECT_EQ(metadata.label, "Test session");
    journal.reset();
    std::filesystem::remove(path);
}

TEST(SessionDatabase, RejectsAStreamingTerminalResponse) {
    const auto path = temporary_path("cha_open_transcript_");
    create_test_database(path);
    auto journal = std::make_unique<SessionJournal>(path);
    const TranscriptEntry prompt = human(1, "Question", 1);
    journal->start_turn(1, prompt);
    EXPECT_THROW(
        journal->complete_turn(1, make_character_entry(
            2,
            "reviewer-id",
            "Reviewer",
            std::string{},
            EntryStatus::streaming,
            1)),
        std::invalid_argument);
    EXPECT_EQ(
        load_transcript_entries(path),
        (std::vector<TranscriptEntry>{prompt}));
    journal.reset();
    std::filesystem::remove(path);
}

TEST(SessionJournal, ReplaysOnlyTheCurrentEpochAfterClear) {
    const auto path = temporary_path("cha_journal_");
    create_test_database(path);
    auto journal = std::make_unique<SessionJournal>(path);
    journal->start_turn(1, human(1, "Old question", 1));
    journal->complete_turn(1, make_character_entry(
        2,
        "reviewer-id",
        "Reviewer",
        "Old answer",
        EntryStatus::complete,
        1));
    journal->clear();
    const TranscriptEntry current_prompt = human(3, "Current question", 2);
    const TranscriptEntry current_response = make_character_entry(
        4,
        "reviewer-id",
        "Reviewer",
        "Current answer",
        EntryStatus::complete,
        2);
    journal->start_turn(2, current_prompt);
    journal->complete_turn(2, current_response);

    EXPECT_EQ(
        load_transcript_entries(path),
        (std::vector<TranscriptEntry>{current_prompt, current_response}));
    journal.reset();
    std::filesystem::remove(path);
}

TEST(SessionJournal, RejectsOutOfOrderEntryIdsWithoutChangingStoredState) {
    const auto path = temporary_path("cha_out_of_order_journal_");
    create_test_database(path);
    auto journal = std::make_unique<SessionJournal>(path);
    const TranscriptEntry later = human(2, "Later ID", 1);
    journal->start_turn(1, later);
    journal->cancel_turn(1, std::nullopt);

    EXPECT_THROW(
        journal->start_turn(2, human(1, "Earlier ID", 2)),
        std::invalid_argument);
    EXPECT_EQ(
        load_transcript_entries(path),
        (std::vector<TranscriptEntry>{later}));
    journal.reset();
    std::filesystem::remove(path);
}

TEST(SessionJournal, RollsBackAnInvalidTerminalTransition) {
    const auto path = temporary_path("cha_rollback_journal_");
    create_test_database(path);
    auto journal = std::make_unique<SessionJournal>(path);
    journal->start_turn(1, human(1, "Question", 1));
    EXPECT_THROW(
        journal->complete_turn(
            1,
            make_character_entry(
                1,
                "guide-id",
                "Guide",
                "Answer",
                EntryStatus::complete,
                1)),
        std::invalid_argument);

    const SessionRestore restored = load_session_state(path);
    ASSERT_EQ(restored.interrupted_turns.size(), 1U);
    expect_same_entry(restored.entries.front(), human(1, "Question", 1));
    journal.reset();
    std::filesystem::remove(path);
}

TEST(SessionJournal, ReplaysIdentifiedTypedTurnOutcomes) {
    const auto path = temporary_path("cha_identified_journal_");
    create_test_database(path);
    auto journal = std::make_unique<SessionJournal>(path);
    journal->start_turn(7, human(1, "First", 7));
    journal->complete_turn(7, make_character_entry(
        2, "guide-id", "Guide", "Answer", EntryStatus::complete, 7));
    journal->start_turn(8, human(3, "Second", 8));
    journal->fail_turn(8, make_error_entry(4, "Unavailable", 8, "guide-id"));

    const SessionRestore restored = load_session_state(path);
    EXPECT_EQ(restored.next_request_id, 9U);
    EXPECT_EQ(restored.next_entry_id, 5U);
    EXPECT_TRUE(restored.interrupted_turns.empty());
    expect_entries(
        restored.entries,
        {
            human(1, "First", 7),
            make_character_entry(2, "guide-id", "Guide", "Answer", EntryStatus::complete, 7),
            human(3, "Second", 8),
            make_error_entry(4, "Unavailable", 8, "guide-id"),
        });
    journal.reset();
    std::filesystem::remove(path);
}

TEST(SessionJournal, RejectsEntriesThatDoNotMatchTheirTurnRecords) {
    const auto path = temporary_path("cha_invalid_turn_entry_");
    create_test_database(path);
    auto journal = std::make_unique<SessionJournal>(path);

    EXPECT_THROW(
        journal->start_turn(7, human(1, "Prompt", 8)),
        std::invalid_argument);
    EXPECT_THROW(
        journal->complete_turn(7, make_character_entry(
            2, "guide-id", "Guide", "Answer", EntryStatus::cancelled, 7)),
        std::invalid_argument);
    EXPECT_THROW(
        journal->cancel_turn(7, make_character_entry(
            2, "guide-id", "Guide", "Answer", EntryStatus::complete, 7)),
        std::invalid_argument);
    EXPECT_THROW(
        journal->fail_turn(7, make_error_entry(2, "Failure", 8, "guide-id")),
        std::invalid_argument);

    journal.reset();
    std::filesystem::remove(path);
}

TEST(SessionJournal, RecognizesAnInterruptedTypedTurn) {
    const auto path = temporary_path("cha_interrupted_journal_");
    create_test_database(path);
    auto journal = std::make_unique<SessionJournal>(path);
    const TranscriptEntry prompt =
        test::human_entry(5, {"human", "You"}, {"guide-id", "Guide"}, "Pending", 12);
    journal->start_turn(12, prompt);

    const SessionRestore restored = load_session_state(path);
    ASSERT_EQ(restored.interrupted_turns.size(), 1U);
    EXPECT_EQ(
        restored.entries,
        (std::vector<TranscriptEntry>{prompt}));
    EXPECT_EQ(
        load_transcript_entries(path),
        (std::vector<TranscriptEntry>{prompt}));
    EXPECT_EQ(restored.interrupted_turns.front().request_id, 12U);
    EXPECT_EQ(restored.interrupted_turns.front().error_entry.kind, EntryKind::error);
    EXPECT_EQ(restored.interrupted_turns.front().error_entry.participant_id, "guide-id");
    EXPECT_EQ(restored.next_request_id, 13U);
    EXPECT_EQ(restored.next_entry_id, 7U);
    journal.reset();
    std::filesystem::remove(path);
}

TEST(SessionDatabase, RejectsANonDatabaseFile) {
    const auto path = temporary_path("cha_old_journal_");
    {
        std::ofstream file(path, std::ios::binary);
        file << R"({"type":"transcript","version":4})" << '\n';
    }

    EXPECT_THROW(SessionJournal journal(path), std::runtime_error);
    EXPECT_THROW((void)load_session_state(path), std::runtime_error);
    std::filesystem::remove(path);
}

TEST(SessionDatabase, JournalDoesNotCreateAMissingDatabase) {
    const auto path = temporary_path("cha_missing_database_");

    EXPECT_THROW(SessionJournal journal(path), std::runtime_error);
    EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(TranscriptValidation, RequiresATargetOnHumanEntriesAndForbidsItElsewhere) {
    TranscriptEntry untargeted = human(1, "No target");
    untargeted.addressed_to.clear();
    EXPECT_THROW(validate_transcript_entry(untargeted), std::invalid_argument);

    TranscriptEntry unnamed = human(1, "No target name");
    unnamed.addressed_to_name.clear();
    EXPECT_THROW(validate_transcript_entry(unnamed), std::invalid_argument);

    TranscriptEntry addressed_character = make_character_entry(
        2, "guide-id", "Guide", "Answer", EntryStatus::complete);
    addressed_character.addressed_to = "reviewer-id";
    addressed_character.addressed_to_name = "Reviewer";
    EXPECT_THROW(validate_transcript_entry(addressed_character), std::invalid_argument);

    TranscriptEntry addressed_notice = make_notice_entry(3, "Notice");
    addressed_notice.addressed_to_name = "Reviewer";
    EXPECT_THROW(validate_transcript_entry(addressed_notice), std::invalid_argument);

    TranscriptEntry addressed_error = make_error_entry(4, "Failure");
    addressed_error.addressed_to = "reviewer-id";
    EXPECT_THROW(validate_transcript_entry(addressed_error), std::invalid_argument);

    EXPECT_NO_THROW(validate_transcript_entry(human(1, "Targeted")));
}

TEST(TranscriptValidation, RejectsAddressingViolationsInMemoryAndInSqlite) {
    TranscriptEntry untargeted = human(1, "No target", 1);
    untargeted.addressed_to.clear();
    untargeted.addressed_to_name.clear();

    Transcript transcript;
    EXPECT_THROW(transcript.add_entry(untargeted), std::invalid_argument);

    const auto path = temporary_path("cha_addressing_");
    create_test_database(path);
    auto journal = std::make_unique<SessionJournal>(path);
    EXPECT_THROW(journal->start_turn(1, untargeted), std::runtime_error);
    EXPECT_TRUE(load_transcript_entries(path).empty());
    journal.reset();
    std::filesystem::remove(path);
}

TEST(SessionDatabase, RoundTripsTheAddressedTargetOfEveryPrompt) {
    const auto path = temporary_path("cha_addressed_round_trip_");
    create_test_database(path);
    auto journal = std::make_unique<SessionJournal>(path);
    journal->start_turn(1, test::human_entry(1, {"human", "You"}, {"ismael", "Ismael"}, "And you?", 1));
    journal->complete_turn(1, make_character_entry(
        2, "ismael", "Ismael", "Call me Ismael.", EntryStatus::complete, 1));

    const std::vector<TranscriptEntry> restored = load_transcript_entries(path);
    ASSERT_EQ(restored.size(), 2U);
    EXPECT_EQ(restored.front().addressed_to, "ismael");
    EXPECT_EQ(restored.front().addressed_to_name, "Ismael");
    EXPECT_TRUE(restored.back().addressed_to.empty());
    EXPECT_TRUE(restored.back().addressed_to_name.empty());
    journal.reset();
    std::filesystem::remove(path);
}

TEST(SessionDatabase, RefusesAVersionOneDatabase) {
    const auto path = temporary_path("cha_version_one_");
    create_test_database(path);
    ASSERT_EQ(raw_execute(path, "PRAGMA user_version = 1"), SQLITE_OK);

    try {
        (void)load_session_state(path);
        FAIL() << "expected the older schema version to be refused";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("Unsupported session database"),
                  std::string::npos)
            << error.what();
    }
    EXPECT_THROW(SessionJournal journal(path), std::runtime_error);
    EXPECT_THROW((void)read_session_database_metadata(path), std::runtime_error);
    std::filesystem::remove(path);
}

TEST(SessionDatabase, StoresAndRestoresEntryCreationTimes) {
    const auto path = temporary_path("cha_created_at_");
    create_test_database(path);
    const TranscriptEntry prompt = human(1, "Question", 1);
    const TranscriptEntry answer = make_character_entry(
        2, "reviewer-id", "Reviewer", "Answer", EntryStatus::complete, 1);
    ASSERT_NE(prompt.created_at, 0);
    ASSERT_NE(answer.created_at, 0);
    {
        SessionJournal journal(path);
        journal.start_turn(1, prompt);
        journal.complete_turn(1, answer);
    }

    EXPECT_EQ(
        load_transcript_entries(path),
        (std::vector<TranscriptEntry>{prompt, answer}));
    std::filesystem::remove(path);
}

TEST(SessionDatabase, MigratesAVersionTwoDatabase) {
    const auto path = temporary_path("cha_migrate_");
    create_test_database(path);
    {
        SessionJournal journal(path);
        journal.start_turn(1, human(1, "Hello", 1));
        journal.complete_turn(1, make_character_entry(
            2, "reviewer-id", "Reviewer", "Hi", EntryStatus::complete, 1));
    }
    // Reshape the database into its version-2 form: no created_at column.
    ASSERT_EQ(
        raw_execute(path, "ALTER TABLE entries DROP COLUMN created_at"),
        SQLITE_OK);
    ASSERT_EQ(raw_execute(path, "PRAGMA user_version = 2"), SQLITE_OK);

    // Read-only paths accept the unmigrated database; restore reads do not
    // happen before migration.
    EXPECT_NO_THROW((void)read_session_database_metadata(path));

    migrate_session_database(path);

    const std::vector<std::string> columns = table_columns(path, "entries");
    EXPECT_NE(
        std::find(columns.begin(), columns.end(), "created_at"),
        columns.end());

    // Rows written before the migration honestly report no recorded time.
    std::vector<TranscriptEntry> restored = load_transcript_entries(path);
    ASSERT_EQ(restored.size(), 2U);
    EXPECT_EQ(restored[0].created_at, 0);
    EXPECT_EQ(restored[1].created_at, 0);

    // Writes after the migration store real timestamps.
    {
        SessionJournal journal(path);
        journal.start_turn(2, human(3, "Again", 2));
    }
    restored = load_transcript_entries(path);
    ASSERT_EQ(restored.size(), 3U);
    EXPECT_NE(restored.back().created_at, 0);

    // Migrating a current database is a no-op.
    migrate_session_database(path);
    EXPECT_EQ(load_transcript_entries(path).size(), 3U);
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
        (std::vector<std::string>{"singleton", "id", "forum", "label"}))
        << "a session belongs to a forum, not to its current characters";
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
        (void)load_session_state(path);
        FAIL() << "expected a turn without a prompt to be rejected";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("turn without exactly one prompt"),
                  std::string::npos)
            << error.what();
    }
    EXPECT_THROW(SessionJournal journal(path), std::runtime_error);
    std::filesystem::remove(path);
}

TEST(SessionDatabase, RecoversAnInterruptedTurnFromItsPersistedPrompt) {
    const auto path = temporary_path("cha_interrupted_target_");
    create_test_database(path);
    {
        SessionJournal journal(path);
        journal.start_turn(1, test::human_entry(1, {"human", "You"}, {"cheburashka", "Cheburashka"}, "Who are you?", 1));
        journal.complete_turn(1, make_character_entry(
            2, "cheburashka", "Cheburashka", "I am Cheburashka.",
            EntryStatus::complete, 1));
        journal.start_turn(2, test::human_entry(3, {"human", "You"}, {"ismael", "Ismael"}, "And you?", 2));
    }

    const SessionRestore restored = load_session_state(path);

    ASSERT_EQ(restored.interrupted_turns.size(), 1U);
    const InterruptedTurn& interrupted = restored.interrupted_turns.front();
    EXPECT_EQ(interrupted.request_id, 2U);
    EXPECT_EQ(interrupted.error_entry.participant_id, "ismael")
        << "the error belongs to the agent the prompt was addressed to";
    EXPECT_EQ(interrupted.error_entry.request_id, 2U);
    EXPECT_EQ(restored.entries.size(), 3U);
    std::filesystem::remove(path);
}

TEST(SessionDatabase, RestoresAndProjectsASessionWhoseForumLostACharacter) {
    const auto path = temporary_path("cha_forum_characters_drift_");
    create_test_database(path);
    {
        SessionJournal journal(path);
        journal.start_turn(1, test::human_entry(1, {"human", "You"}, {"cheburashka", "Cheburashka"}, "Who are you?", 1));
        journal.complete_turn(1, make_character_entry(
            2, "cheburashka", "Cheburashka", "I am Cheburashka.",
            EntryStatus::complete, 1));
        journal.start_turn(2, test::human_entry(3, {"human", "You"}, {"ismael", "Ismael"}, "And you?", 2));
        journal.complete_turn(2, make_character_entry(
            4, "ismael", "Ismael", "Call me Ismael.", EntryStatus::complete, 2));
    }

    // Cheburashka has left the forum; only Ismael remains.
    const SessionRestore restored = load_session_state(path);

    EXPECT_TRUE(restored.interrupted_turns.empty());
    ASSERT_EQ(restored.entries.size(), 4U);
    EXPECT_EQ(restored.entries.front().addressed_to, "cheburashka");
    EXPECT_EQ(restored.entries[1].display_name, "Cheburashka");

    EXPECT_EQ(
        project_model_context(
            restored.entries,
            std::nullopt,
            {},
            "Ismael system",
            "ismael"),
        (std::vector<ModelMessage>{
            {ModelRole::system, "Ismael system"},
            {ModelRole::persona,
             "Shared chat history (JSONL):\n"
             R"({"kind":"human","speaker":"You","addressed_to":"Cheburashka","text":"Who are you?"})"
             "\n"
             R"({"kind":"character","speaker":"Cheburashka","text":"I am Cheburashka."})"},
            {ModelRole::persona, "from You:\nAnd you?"},
            {ModelRole::assistant, "Call me Ismael."},
        }));
    std::filesystem::remove(path);
}

} // namespace
} // namespace cha
