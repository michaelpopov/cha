#include "support/test_workspace.h"
#include "support/test_transcript.h"
#include "session/not_found_error.h"
#include "session/session_database.h"
#include "workspace/builtins.h"
#include "workspace/workspace_runtime.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

namespace cha {
namespace {

TemporarySessionSeed welcome_seed() {
    return {{std::string(entrance_id), std::string(welcome_id)}, std::string(welcome_name)};
}

void add_writer_forum(const test::TestWorkspace& fixture) {
    fixture.add_character("writer", "Writer");
    fixture.add_forum("writers", "Writers", "writer");
}

TEST(WorkspaceRuntime, PublishesACompleteReplacementGeneration) {
    test::TestWorkspace fixture;
    WorkspaceRuntime runtime(fixture.root(), welcome_seed());
    const auto before = runtime.snapshot();

    fixture.add_persona("writer_persona", "Writer Persona");
    add_writer_forum(fixture);
    runtime.reload();

    const auto after = runtime.snapshot();
    EXPECT_NE(after, before);
    EXPECT_NE(after->model->find_character("writer"), nullptr);
    EXPECT_NE(after->model->find_persona("writer_persona"), nullptr);
    EXPECT_NE(after->model->find_forum("writers"), nullptr);
    EXPECT_NO_THROW((void)after->sessions->create("writers", "Draft"));

    // The prior generation remains a valid immutable view while a request or
    // live session still retains it.
    EXPECT_EQ(before->model->find_character("writer"), nullptr);
    EXPECT_THROW((void)before->sessions->list("writers"), ForumNotFoundError);
}

TEST(WorkspaceRuntime, KeepsTheCurrentGenerationWhenReloadValidationFails) {
    test::TestWorkspace fixture;
    WorkspaceRuntime runtime(fixture.root(), welcome_seed());
    const auto before = runtime.snapshot();

    fixture.write_character_config("display_name =\n");
    EXPECT_THROW(runtime.reload(), std::exception);

    const auto after = runtime.snapshot();
    EXPECT_EQ(after, before);
    EXPECT_NE(after->model->find_character("guide"), nullptr);
}

TEST(WorkspaceRuntime, ExportsAndBootstrapsAStoredSessionOnReload) {
    test::TestWorkspace fixture;
    WorkspaceRuntime runtime(fixture.root(), welcome_seed());
    const StoredSession created =
        runtime.snapshot()->sessions->create("lobby", "O'Brien");
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

    runtime.reload();
    ASSERT_TRUE(std::filesystem::is_regular_file(snapshot));

    // An active local database is authoritative, even over an existing SQL
    // file. The next reload must replace this invalid snapshot with its dump.
    std::ofstream(snapshot, std::ios::binary | std::ios::trunc) << "not SQL";
    runtime.reload();

    ASSERT_TRUE(std::filesystem::remove(created.database_path));
    runtime.reload();

    const std::vector<StoredSession> sessions =
        runtime.snapshot()->sessions->list("lobby");
    ASSERT_EQ(sessions.size(), 1U);
    EXPECT_EQ(sessions.front().identity, created.identity);
    EXPECT_EQ(sessions.front().label, "O'Brien");
    const PreparedSession prepared =
        runtime.snapshot()->sessions->prepare(created.identity);
    EXPECT_EQ(
        prepared.restore.entries,
        (std::vector<TranscriptEntry>{prompt, response}));
}

TEST(WorkspaceRuntime, DoesNotBootstrapADeletedSessionSnapshot) {
    test::TestWorkspace fixture;
    WorkspaceRuntime runtime(fixture.root(), welcome_seed());
    const StoredSession created =
        runtime.snapshot()->sessions->create("lobby", "Deleted");
    std::filesystem::path snapshot = created.database_path;
    snapshot.replace_extension(".sql");
    runtime.reload();
    ASSERT_TRUE(std::filesystem::is_regular_file(snapshot));

    runtime.snapshot()->sessions->move_to_deleted(created.identity);
    runtime.reload();

    EXPECT_FALSE(std::filesystem::exists(created.database_path));
    EXPECT_TRUE(std::filesystem::is_regular_file(snapshot));
    EXPECT_TRUE(runtime.snapshot()->sessions->list("lobby").empty());
}

TEST(WorkspaceRuntime, DoesNotPublishAPartialDatabaseForInvalidSql) {
    test::TestWorkspace fixture;
    WorkspaceRuntime runtime(fixture.root(), welcome_seed());
    const auto before = runtime.snapshot();
    const std::filesystem::path sessions =
        fixture.root() / "forums" / "lobby" / "sessions";
    std::filesystem::create_directories(sessions);
    std::ofstream(sessions / "broken.sql") << "this is not SQL";

    EXPECT_THROW(runtime.reload(), std::exception);

    EXPECT_EQ(runtime.snapshot(), before);
    EXPECT_FALSE(std::filesystem::exists(sessions / "broken.sqlite3"));
}

TEST(WorkspaceRuntime, SkipsSnapshotExportForAnUnreadableDatabase) {
    test::TestWorkspace fixture;
    WorkspaceRuntime runtime(fixture.root(), welcome_seed());
    add_writer_forum(fixture);
    const std::filesystem::path sessions =
        fixture.root() / "forums" / "writers" / "sessions";
    std::filesystem::create_directories(sessions);
    std::ofstream(sessions / "broken.sqlite3") << "this is not a database";
    const std::filesystem::path healthy = sessions / "draft.sqlite3";
    ASSERT_TRUE(create_session_database(
        healthy, {.id = "draft", .forum = "writers", .label = "Draft"}));

    runtime.reload();

    // The corrupt file is what listing already shows as invalid, so it only
    // loses its own snapshot; every other database is still exported.
    EXPECT_FALSE(std::filesystem::exists(sessions / "broken.sql"));
    EXPECT_TRUE(std::filesystem::is_regular_file(sessions / "draft.sql"));
}

TEST(WorkspaceRuntime, BootstrapsSnapshotsForAForumAddedByTheReload) {
    test::TestWorkspace fixture;
    WorkspaceRuntime runtime(fixture.root(), welcome_seed());
    add_writer_forum(fixture);
    const std::filesystem::path sessions =
        fixture.root() / "forums" / "writers" / "sessions";
    std::filesystem::create_directories(sessions);
    const std::filesystem::path database = sessions / "draft.sqlite3";
    const std::filesystem::path snapshot = sessions / "draft.sql";
    ASSERT_TRUE(create_session_database(
        database, {.id = "draft", .forum = "writers", .label = "Draft"}));
    export_session_database_sql(
        database, snapshot, {.forum_id = "writers", .session_id = "draft"});
    ASSERT_TRUE(std::filesystem::remove(database));

    runtime.reload();

    ASSERT_TRUE(std::filesystem::is_regular_file(database));
    const std::vector<StoredSession> stored =
        runtime.snapshot()->sessions->list("writers");
    ASSERT_EQ(stored.size(), 1U);
    EXPECT_EQ(stored.front().label, "Draft");
}

} // namespace
} // namespace cha
