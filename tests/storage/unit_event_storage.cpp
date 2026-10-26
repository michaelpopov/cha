#include "storage/event_storage.h"
#include "storage/session_timestamp.h"
#include "storage/workspace_session_database.h"
#include "support/test_workspace.h"
#include "workspace/workspace_config_store.h"

#include <gtest/gtest.h>

namespace cha {
namespace {

using Database = storage::SqliteDatabase;

class EventStorageTest : public testing::Test {
protected:
    void SetUp() override {
        database_ = std::make_unique<Database>(workspace_.root() / "events.sqlite3",
            Database::Mode::read_write_create);
        storage::SqliteTransaction transaction(*database_);
        create_event_history_schema(*database_);
        transaction.commit();
    }
    test::TestWorkspace workspace_;
    std::unique_ptr<Database> database_;
};

TEST_F(EventStorageTest, EmptyReadAndAllSeveritiesWithUnicode) {
    EXPECT_TRUE(event_list(*database_).empty());
    const auto before = session_timestamp();
    for (auto severity : {EventSeverity::info, EventSeverity::success,
             EventSeverity::warning, EventSeverity::error}) {
        event_append(*database_, severity, "Café 日本語 😀\n<plain text>");
    }
    const auto events = event_list(*database_);
    ASSERT_EQ(events.size(), 4U);
    for (std::size_t i = 0; i < events.size(); ++i) {
        EXPECT_EQ(events[i].id, 4 - static_cast<std::int64_t>(i));
        EXPECT_GE(events[i].recorded_at, before);
        EXPECT_LE(events[i].recorded_at, session_timestamp());
        EXPECT_EQ(events[i].message, "Café 日本語 😀\n<plain text>");
    }
    const nlohmann::json encoded = events;
    EXPECT_EQ(encoded[0]["severity"], "error");
    EXPECT_EQ(encoded[1]["severity"], "warning");
    EXPECT_EQ(encoded[2]["severity"], "success");
    EXPECT_EQ(encoded[3]["severity"], "info");
    EXPECT_EQ(encoded[0].size(), 4U);
}

TEST_F(EventStorageTest, UsesIdOrderWhenTimestampsAreEqualOrMoveBackwards) {
    for (int i = 0; i < 3; ++i) event_append(*database_, EventSeverity::info, "Entry");
    database_->execute("UPDATE event_history SET recorded_at = CASE WHEN id = 3 THEN 0 ELSE 10 END");
    const auto events = event_list(*database_);
    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(events[0].id, 3);
    EXPECT_EQ(events[0].recorded_at, 0);
    EXPECT_EQ(events[1].id, 2);
    EXPECT_EQ(events[2].id, 1);
}

TEST_F(EventStorageTest, TrimsOnlyBeyondTwoHundredAndReadsDoNotWrite) {
    for (int i = 1; i <= 200; ++i) event_append(*database_, EventSeverity::info, std::to_string(i));
    ASSERT_EQ(event_list(*database_).size(), 200U);
    EXPECT_EQ(event_list(*database_).back().id, 1);
    for (int i = 201; i <= 405; ++i) event_append(*database_, EventSeverity::success, std::to_string(i));
    database_->execute("UPDATE event_history SET recorded_at = 0");
    const auto before = sqlite3_total_changes(database_->handle());
    database_->execute("PRAGMA query_only = ON");
    const auto events = event_list(*database_);
    ASSERT_EQ(events.size(), 200U);
    EXPECT_EQ(events.front().id, 405);
    EXPECT_EQ(events.back().id, 206);
    EXPECT_EQ(events.back().recorded_at, 0);
    EXPECT_EQ(sqlite3_total_changes(database_->handle()), before);
}

TEST_F(EventStorageTest, InsertAndTrimFailuresRollBackEntireAppend) {
    for (int i = 0; i < 200; ++i) event_append(*database_, EventSeverity::info, "Existing");
    const nlohmann::json original = event_list(*database_);
    database_->execute("CREATE TEMP TRIGGER fail_insert BEFORE INSERT ON event_history "
        "BEGIN SELECT RAISE(ABORT, 'insert failed'); END");
    EXPECT_THROW(event_append(*database_, EventSeverity::error, "New"), std::runtime_error);
    EXPECT_EQ(nlohmann::json(event_list(*database_)), original);
    database_->execute("DROP TRIGGER fail_insert");
    database_->execute("CREATE TEMP TRIGGER fail_trim BEFORE DELETE ON event_history "
        "BEGIN SELECT RAISE(ABORT, 'trim failed'); END");
    EXPECT_THROW(event_append(*database_, EventSeverity::error, "New"), std::runtime_error);
    EXPECT_EQ(nlohmann::json(event_list(*database_)), original);
    database_->execute("DROP TRIGGER fail_trim");
    event_append(*database_, EventSeverity::success, "New");
    EXPECT_EQ(event_list(*database_).front().id, 201);
}

TEST_F(EventStorageTest, RejectsInvalidInputAndDamagedStoredRecords) {
    EXPECT_THROW(event_append(*database_, static_cast<EventSeverity>(99), "Text"), std::invalid_argument);
    EXPECT_THROW(event_append(*database_, EventSeverity::info, ""), std::invalid_argument);
    EXPECT_THROW(event_append(*database_, EventSeverity::info, "\xff"), std::runtime_error);
    EXPECT_TRUE(event_list(*database_).empty());
    EXPECT_THROW(database_->execute("INSERT INTO event_history VALUES (1, 0, 'info', X'01')"), std::runtime_error);
    event_append(*database_, EventSeverity::info, "Valid");
    database_->execute("PRAGMA ignore_check_constraints = ON");
    for (const auto sql : {
             "UPDATE event_history SET severity = 'unknown'",
             "UPDATE event_history SET severity = 'info', recorded_at = -1",
             "UPDATE event_history SET recorded_at = 0, message = ''",
             "UPDATE event_history SET message = CAST(X'FF' AS TEXT)",
             "UPDATE event_history SET message = 'valid', id = 9007199254740992"}) {
        database_->execute(sql);
        EXPECT_ANY_THROW(event_list(*database_));
    }
}

TEST(EventStore, FreshAndOlderV2VaultsCreateSchemaAndPreserveHistoryOnReopen) {
    for (bool older : {false, true}) {
        test::TestWorkspace workspace;
        const auto path = older ? test::import_test_database(workspace.root())
                                : workspace.root() / "fresh.sqlite3";
        if (!older) {
            create_empty_workspace_session_database(path);
            (void)import_workspace_configuration(workspace.root(), path);
        }
        {
            Database database(path, Database::Mode::read_write);
            database.execute("DROP TABLE IF EXISTS event_history");
            EXPECT_EQ(database.pragma_integer("user_version"), 2);
        }
        nlohmann::json saved;
        {
            auto store = WorkspaceConfigStore::open(path);
            EXPECT_TRUE(store->list_events().empty());
            const auto revision = store->config_revision();
            const auto snapshot = store->snapshot();
            store->append_event(EventSeverity::success, "Saved 日本語");
            EXPECT_EQ(store->config_revision(), revision);
            EXPECT_EQ(store->snapshot().get(), snapshot.get());
            saved = store->list_events();
        }
        {
            auto store = WorkspaceConfigStore::open(path);
            EXPECT_EQ(nlohmann::json(store->list_events()), saved);
        }
        protect_workspace_session_database(path, "secret");
        {
            auto store = WorkspaceConfigStore::open(path, "secret");
            EXPECT_EQ(nlohmann::json(store->list_events()), saved);
            store->append_event(EventSeverity::info, "Protected entry");
            saved = store->list_events();
        }
        auto store = WorkspaceConfigStore::open(path, "secret");
        EXPECT_EQ(nlohmann::json(store->list_events()), saved);
    }
}

} // namespace
} // namespace cha
