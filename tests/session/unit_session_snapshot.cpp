#include "session/session_snapshot.h"

#include "session/session_database.h"
#include "session/session_repository.h"
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

void reload_with_session_bootstrap(const std::filesystem::path& root) {
    Workspace candidate = Workspace::load(root);
    bootstrap_sessions_from_sql(candidate);
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
    reload_with_session_bootstrap(fixture_.root());

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

    EXPECT_THROW(reload_with_session_bootstrap(fixture_.root()), std::exception);

    EXPECT_EQ(getws(), before);
    EXPECT_NE(getws()->find_character("guide"), nullptr);
}

TEST_F(SessionSnapshotTest, DoesNotCreateOrRefreshSqlForAStoredSession) {
    const StoredSession created = sessions_->create("lobby", "Private");
    std::filesystem::path snapshot = created.database_path;
    snapshot.replace_extension(".sql");

    reload_with_session_bootstrap(fixture_.root());
    ASSERT_FALSE(std::filesystem::exists(snapshot));

    std::ofstream(snapshot) << "existing snapshot";
    reload_with_session_bootstrap(fixture_.root());

    std::ifstream input(snapshot);
    std::string contents;
    std::getline(input, contents);
    EXPECT_EQ(contents, "existing snapshot");
}

TEST_F(SessionSnapshotTest, DoesNotBootstrapADeletedSession) {
    const StoredSession created = sessions_->create("lobby", "Deleted");
    std::filesystem::path snapshot = created.database_path;
    snapshot.replace_extension(".sql");
    export_session_database_sql(
        created.database_path, snapshot, created.identity);
    ASSERT_TRUE(std::filesystem::is_regular_file(snapshot));

    sessions_->move_to_deleted(created.identity);
    reload_with_session_bootstrap(fixture_.root());

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

    EXPECT_THROW(reload_with_session_bootstrap(fixture_.root()), std::exception);

    EXPECT_EQ(getws(), before);
    EXPECT_FALSE(std::filesystem::exists(directory / "broken.sqlite3"));
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

    reload_with_session_bootstrap(fixture_.root());

    ASSERT_TRUE(std::filesystem::is_regular_file(database));
    const std::vector<StoredSession> stored = sessions_->list("writers");
    ASSERT_EQ(stored.size(), 1U);
    EXPECT_EQ(stored.front().label, "Draft");
}

} // namespace
} // namespace cha
