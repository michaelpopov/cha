#include "session/session_snapshot.h"

#include "session/not_found_error.h"
#include "session/session_database.h"
#include "session/session_repository.h"
#include "support/test_transcript.h"
#include "support/test_workspace.h"
#include "workspace/builtins.h"
#include "workspace/workspace.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace cha {
namespace {

TemporarySessionSeed welcome_seed() {
    return {
        {std::string(entrance_id), std::string(welcome_id)},
        std::string(welcome_name),
    };
}

void add_writer_forum(const test::TestWorkspace& fixture) {
    fixture.add_character("writer", "Writer");
    fixture.add_forum("writers", "Writers", "writer");
}

void reload_with_snapshots(const std::filesystem::path& root) {
    Workspace candidate = Workspace::load(root);
    export_and_bootstrap_sessions(candidate);
    loadws(std::move(candidate));
}

class SessionSnapshotTest : public ::testing::Test {
protected:
    SessionSnapshotTest() {
        loadws(fixture_.root());
        sessions_ = std::make_unique<SessionRepository>(
            fixture_.root(), welcome_seed());
    }

    test::TestWorkspace fixture_;
    std::unique_ptr<SessionRepository> sessions_;
};

TEST_F(SessionSnapshotTest, PublishesACompleteReplacementWorkspace) {
    const std::shared_ptr<const Workspace> before = getws();

    fixture_.add_persona("writer_persona", "Writer Persona");
    add_writer_forum(fixture_);
    reload_with_snapshots(fixture_.root());

    const std::shared_ptr<const Workspace> after = getws();
    EXPECT_NE(after, before);
    EXPECT_NE(after->find_character("writer"), nullptr);
    EXPECT_NE(after->find_persona("writer_persona"), nullptr);
    EXPECT_NE(after->find_forum("writers"), nullptr);
    EXPECT_NO_THROW((void)sessions_->create("writers", "Draft"));

    EXPECT_EQ(before->find_character("writer"), nullptr);
}

TEST_F(SessionSnapshotTest, KeepsCurrentWorkspaceWhenCandidateLoadFails) {
    const std::shared_ptr<const Workspace> before = getws();
    fixture_.write_character_config("display_name =\n");

    EXPECT_THROW(reload_with_snapshots(fixture_.root()), std::exception);

    EXPECT_EQ(getws(), before);
    EXPECT_NE(getws()->find_character("guide"), nullptr);
}

TEST_F(SessionSnapshotTest, ExportsAndBootstrapsAStoredSession) {
    const StoredSession created = sessions_->create("lobby", "O'Brien");
    const TranscriptEntry prompt = test::human_entry(
        1, {"human", "You"}, {"guide", "Guide"}, "What's new?", 1);
    const TranscriptEntry response = make_character_entry(
        2, "guide", "Guide", "Everything's portable.",
        EntryStatus::complete, 1);
    {
        SessionJournal journal(created.database_path);
        journal.start_turn(1, prompt);
        journal.complete_turn(1, response);
    }
    std::filesystem::path snapshot = created.database_path;
    snapshot.replace_extension(".sql");

    reload_with_snapshots(fixture_.root());
    ASSERT_TRUE(std::filesystem::is_regular_file(snapshot));

    std::ofstream(snapshot, std::ios::binary | std::ios::trunc) << "not SQL";
    reload_with_snapshots(fixture_.root());

    ASSERT_TRUE(std::filesystem::remove(created.database_path));
    reload_with_snapshots(fixture_.root());

    const std::vector<StoredSession> stored = sessions_->list("lobby");
    ASSERT_EQ(stored.size(), 1U);
    EXPECT_EQ(stored.front().identity, created.identity);
    EXPECT_EQ(stored.front().label, "O'Brien");
    const PreparedSession prepared = sessions_->prepare(created.identity);
    EXPECT_EQ(
        prepared.restore.entries,
        (std::vector<TranscriptEntry>{prompt, response}));
}

TEST_F(SessionSnapshotTest, DoesNotBootstrapADeletedSession) {
    const StoredSession created = sessions_->create("lobby", "Deleted");
    std::filesystem::path snapshot = created.database_path;
    snapshot.replace_extension(".sql");
    reload_with_snapshots(fixture_.root());
    ASSERT_TRUE(std::filesystem::is_regular_file(snapshot));

    sessions_->move_to_deleted(created.identity);
    reload_with_snapshots(fixture_.root());

    EXPECT_FALSE(std::filesystem::exists(created.database_path));
    EXPECT_TRUE(std::filesystem::is_regular_file(snapshot));
    EXPECT_TRUE(sessions_->list("lobby").empty());
}

TEST_F(SessionSnapshotTest, InvalidSqlDoesNotPublishOrLeaveAPartialDatabase) {
    const std::shared_ptr<const Workspace> before = getws();
    const std::filesystem::path directory =
        fixture_.root() / "forums" / "lobby" / "sessions";
    std::filesystem::create_directories(directory);
    std::ofstream(directory / "broken.sql") << "this is not SQL";

    EXPECT_THROW(reload_with_snapshots(fixture_.root()), std::exception);

    EXPECT_EQ(getws(), before);
    EXPECT_FALSE(std::filesystem::exists(directory / "broken.sqlite3"));
}

TEST_F(SessionSnapshotTest, SkipsAnUnreadableDatabaseAndExportsHealthyOnes) {
    add_writer_forum(fixture_);
    const std::filesystem::path directory =
        fixture_.root() / "forums" / "writers" / "sessions";
    std::filesystem::create_directories(directory);
    std::ofstream(directory / "broken.sqlite3") << "not a database";
    const std::filesystem::path healthy = directory / "draft.sqlite3";
    ASSERT_TRUE(create_session_database(
        healthy, {.id = "draft", .forum = "writers", .label = "Draft"}));

    reload_with_snapshots(fixture_.root());

    EXPECT_FALSE(std::filesystem::exists(directory / "broken.sql"));
    EXPECT_TRUE(std::filesystem::is_regular_file(directory / "draft.sql"));
}

TEST_F(SessionSnapshotTest, BootstrapsSnapshotsForANewForum) {
    add_writer_forum(fixture_);
    const std::filesystem::path directory =
        fixture_.root() / "forums" / "writers" / "sessions";
    std::filesystem::create_directories(directory);
    const std::filesystem::path database = directory / "draft.sqlite3";
    const std::filesystem::path snapshot = directory / "draft.sql";
    ASSERT_TRUE(create_session_database(
        database, {.id = "draft", .forum = "writers", .label = "Draft"}));
    export_session_database_sql(
        database, snapshot, {.forum_id = "writers", .session_id = "draft"});
    ASSERT_TRUE(std::filesystem::remove(database));

    reload_with_snapshots(fixture_.root());

    ASSERT_TRUE(std::filesystem::is_regular_file(database));
    const std::vector<StoredSession> stored = sessions_->list("writers");
    ASSERT_EQ(stored.size(), 1U);
    EXPECT_EQ(stored.front().label, "Draft");
}

} // namespace
} // namespace cha
