#include "session/session_repository.h"

#include "session/not_found_error.h"
#include "session/session_database.h"
#include "session/session_delete_conflict.h"
#include "session/session_lease.h"
#include "support/test_transcript.h"
#include "support/test_workspace.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace cha {
namespace {

TEST(SessionIdentity, EqualityAndOrderingUseForumAndSessionIds) {
    const SessionIdentity first{"alpha", "one"};
    const SessionIdentity same{"alpha", "one"};
    const SessionIdentity different_forum{"beta", "one"};
    const SessionIdentity different_session{"alpha", "two"};

    EXPECT_EQ(first, same);
    EXPECT_NE(first, different_forum);
    EXPECT_NE(first, different_session);

    std::map<SessionIdentity, int> ordered{
        {different_forum, 3}, {different_session, 2}, {first, 1}};
    std::vector<SessionIdentity> keys;
    for (const auto& [identity, ignored] : ordered) {
        (void)ignored;
        keys.push_back(identity);
    }
    EXPECT_EQ(keys, (std::vector<SessionIdentity>{first, different_session, different_forum}));
}

constexpr std::string_view temporary_forum = "temporary-forum";
constexpr std::string_view temporary_session = "temporary-session";

class SessionRepositoryTest : public ::testing::Test {
protected:
    std::filesystem::path sessions_directory() const {
        return fixture_.root() / "forums" / "lobby" / "sessions";
    }

    SessionRepository make_repository() const {
        return SessionRepository(
            {{"lobby", sessions_directory()}},
            {{std::string(temporary_forum), std::string(temporary_session)}, "Welcome"});
    }

    static SessionIdentity temporary_identity() {
        return {std::string(temporary_forum), std::string(temporary_session)};
    }

    test::TestWorkspace fixture_;
};

TEST_F(SessionRepositoryTest, CreatesListsValidatesAndPreparesAPersistentSession) {
    const SessionRepository repository = make_repository();

    const StoredSession created = repository.create("lobby", "Stored");
    EXPECT_EQ(created.identity.forum_id, "lobby");
    EXPECT_EQ(created.label, "Stored");
    EXPECT_TRUE(created.error.empty());

    const std::vector<StoredSession> listed = repository.list("lobby");
    ASSERT_EQ(listed.size(), 1U);
    EXPECT_EQ(listed.front().identity, created.identity);
    EXPECT_EQ(listed.front().label, "Stored");
    EXPECT_EQ(listed.front().updated_at, created.updated_at);

    EXPECT_NO_THROW(repository.validate(created.identity));

    const PreparedSession prepared = repository.prepare(created.identity);
    EXPECT_EQ(prepared.identity, created.identity);
    EXPECT_EQ(prepared.label, "Stored");
    EXPECT_EQ(prepared.database_path, sessions_directory() / (created.identity.session_id + ".sqlite3"));
    EXPECT_TRUE(prepared.lease.active());
    EXPECT_TRUE(prepared.restore.entries.empty());
}

TEST_F(SessionRepositoryTest, LabelsAnEmptyRequestWithItsSessionIdAndOrdersById) {
    const SessionRepository repository = make_repository();
    const StoredSession first = repository.create("lobby", "");
    const StoredSession second = repository.create("lobby", "Second");
    EXPECT_EQ(first.label, first.identity.session_id);

    std::vector<std::string> ids;
    for (const StoredSession& entry : repository.list("lobby")) {
        ids.push_back(entry.identity.session_id);
    }
    ASSERT_EQ(ids.size(), 2U);
    std::vector<std::string> expected{first.identity.session_id, second.identity.session_id};
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(ids, expected);
}

TEST_F(SessionRepositoryTest, RenamesStoredMetadataWithoutChangingIdentity) {
    const SessionRepository repository = make_repository();
    const StoredSession created = repository.create("lobby", "Before");

    const StoredSession renamed = repository.rename(created.identity, "After ✓");

    EXPECT_EQ(renamed.identity, created.identity);
    EXPECT_EQ(renamed.label, "After ✓");
    ASSERT_EQ(repository.list("lobby").size(), 1U);
    EXPECT_EQ(repository.list("lobby").front().label, "After ✓");
    EXPECT_EQ(repository.prepare(created.identity).label, "After ✓");
}

TEST_F(SessionRepositoryTest, AppliesTheSharedLabelPolicyToCreateAndRename) {
    const SessionRepository repository = make_repository();
    EXPECT_THROW((void)repository.create("lobby", " leading"), std::invalid_argument);
    EXPECT_THROW((void)repository.create("lobby", "line\nbreak"), std::invalid_argument);
    EXPECT_THROW((void)repository.create("lobby", std::string(201, 'x')), std::invalid_argument);

    const StoredSession created = repository.create("lobby", "Valid");
    EXPECT_THROW((void)repository.rename(created.identity, ""), std::invalid_argument);
    EXPECT_THROW((void)repository.rename(created.identity, "trailing "), std::invalid_argument);
    EXPECT_EQ(repository.prepare(created.identity).label, "Valid");
}

TEST_F(SessionRepositoryTest, MovesDeletedDatabaseButLeavesItsCompanionLock) {
    const SessionRepository repository = make_repository();
    const StoredSession created = repository.create("lobby", "Archived");
    const std::filesystem::path lock = SessionLease::companion_path(created.database_path);
    ASSERT_TRUE(std::filesystem::exists(lock));

    repository.move_to_deleted(created.identity);

    EXPECT_FALSE(std::filesystem::exists(created.database_path));
    EXPECT_TRUE(std::filesystem::exists(
        sessions_directory() / "deleted" / created.database_path.filename()));
    EXPECT_TRUE(std::filesystem::exists(lock));
    EXPECT_TRUE(repository.list("lobby").empty());
    EXPECT_THROW((void)repository.prepare(created.identity), SessionNotFoundError);
}

TEST_F(SessionRepositoryTest, DeletesCorruptDatabasesAndNeverReplacesAnArchive) {
    const SessionRepository repository = make_repository();
    const StoredSession corrupt = repository.create("lobby", "Corrupt");
    {
        std::ofstream file(corrupt.database_path, std::ios::binary | std::ios::trunc);
        file << "not SQLite";
    }
    EXPECT_NO_THROW(repository.move_to_deleted(corrupt.identity));

    const StoredSession conflict = repository.create("lobby", "Conflict");
    const std::filesystem::path destination =
        sessions_directory() / "deleted" / conflict.database_path.filename();
    std::filesystem::copy_file(
        sessions_directory() / "deleted" / corrupt.database_path.filename(),
        destination);
    EXPECT_THROW(
        repository.move_to_deleted(conflict.identity),
        SessionDeleteConflictError);
    EXPECT_TRUE(std::filesystem::exists(conflict.database_path));
}

TEST_F(SessionRepositoryTest, PrepareMigratesAVersionTwoSessionDatabase) {
    const SessionRepository repository = make_repository();
    const StoredSession created = repository.create("lobby", "Old");

    // Reshape the database into its version-2 form: no created_at column.
    const std::string path_string = created.database_path.string();
    sqlite3* handle = nullptr;
    ASSERT_EQ(sqlite3_open(path_string.c_str(), &handle), SQLITE_OK);
    ASSERT_EQ(
        sqlite3_exec(
            handle,
            "ALTER TABLE entries DROP COLUMN created_at; PRAGMA user_version = 2",
            nullptr,
            nullptr,
            nullptr),
        SQLITE_OK);
    sqlite3_close(handle);

    // Read-only paths accept the unmigrated database.
    EXPECT_NO_THROW(repository.validate(created.identity));

    // Preparing migrates the database in place before the restore read.
    const PreparedSession prepared = repository.prepare(created.identity);
    EXPECT_TRUE(prepared.restore.entries.empty());

    std::int64_t version = 0;
    ASSERT_EQ(sqlite3_open(path_string.c_str(), &handle), SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    ASSERT_EQ(
        sqlite3_prepare_v2(handle, "PRAGMA user_version", -1, &statement, nullptr),
        SQLITE_OK);
    ASSERT_EQ(sqlite3_step(statement), SQLITE_ROW);
    version = sqlite3_column_int64(statement, 0);
    sqlite3_finalize(statement);
    sqlite3_close(handle);
    EXPECT_EQ(version, 3);
}

TEST_F(SessionRepositoryTest, RenameAndDeleteRejectBusyAndTemporarySessions) {
    const SessionRepository repository = make_repository();
    const StoredSession created = repository.create("lobby", "Held");
    const PreparedSession held = repository.prepare(created.identity);
    EXPECT_THROW((void)repository.rename(created.identity, "Later"), SessionBusyError);
    EXPECT_THROW(repository.move_to_deleted(created.identity), SessionBusyError);

    EXPECT_THROW(
        (void)repository.rename(temporary_identity(), "Other"),
        ForumNotFoundError);
    EXPECT_THROW(
        repository.move_to_deleted(temporary_identity()),
        ForumNotFoundError);
}

TEST_F(SessionRepositoryTest, ListsAnEmptyForumWithoutCreatingItsDirectory) {
    const SessionRepository repository = make_repository();
    EXPECT_TRUE(repository.list("lobby").empty());
    EXPECT_FALSE(std::filesystem::exists(sessions_directory()));
}

TEST_F(SessionRepositoryTest, KeepsADamagedDatabaseListedButRejectsItStrictly) {
    const SessionRepository repository = make_repository();
    const StoredSession created = repository.create("lobby", "Broken later");
    {
        std::ofstream database(
            sessions_directory() / (created.identity.session_id + ".sqlite3"),
            std::ios::binary | std::ios::trunc);
        database << "not SQLite";
    }

    const std::vector<StoredSession> listed = repository.list("lobby");
    ASSERT_EQ(listed.size(), 1U);
    EXPECT_EQ(listed.front().identity, created.identity);
    EXPECT_EQ(listed.front().label, created.identity.session_id + " [invalid database]");
    EXPECT_FALSE(listed.front().error.empty());

    EXPECT_THROW(repository.validate(created.identity), std::runtime_error);
    EXPECT_THROW((void)repository.prepare(created.identity), std::runtime_error);
}

TEST_F(SessionRepositoryTest, CreatesWithoutInitializingAProvider) {
    // A network-mode provider with no configured model would have to discover
    // one during controller construction. Creation must not do that work.
    fixture_.write_character_defaults(
        "host = \"127.0.0.1\"\nport = 1\nmode = \"net\"\n");
    const SessionRepository repository = make_repository();

    const StoredSession created = repository.create("lobby", "Unopened");
    EXPECT_FALSE(created.identity.session_id.empty());
    EXPECT_EQ(repository.list("lobby").size(), 1U);
}

TEST_F(SessionRepositoryTest, RejectsAForumIdThatIsNotOneSafePathComponent) {
    const SessionRepository repository = make_repository();
    EXPECT_THROW((void)repository.create("../missing", "Escaping"), ForumNotFoundError);
    EXPECT_TRUE(repository.list("lobby").empty());
}

TEST_F(SessionRepositoryTest, ReportsUnknownForumsAndSessionsSeparately) {
    const SessionRepository repository = make_repository();

    EXPECT_THROW((void)repository.list("absent"), ForumNotFoundError);
    EXPECT_THROW((void)repository.create("absent", "Label"), ForumNotFoundError);
    EXPECT_THROW(repository.validate({"absent", "any"}), ForumNotFoundError);
    EXPECT_THROW((void)repository.prepare({"absent", "any"}), ForumNotFoundError);

    EXPECT_THROW(repository.validate({"lobby", "absent"}), SessionNotFoundError);
    EXPECT_THROW((void)repository.prepare({"lobby", "absent"}), SessionNotFoundError);
}

TEST_F(SessionRepositoryTest, OffersOneTemporaryRowAndRefusesToCreateBesideIt) {
    const SessionRepository repository = make_repository();

    const std::vector<StoredSession> listed = repository.list(temporary_forum);
    ASSERT_EQ(listed.size(), 1U);
    EXPECT_EQ(listed.front().identity, temporary_identity());
    EXPECT_EQ(listed.front().label, "Welcome");
    EXPECT_TRUE(listed.front().error.empty());

    EXPECT_NO_THROW(repository.validate(temporary_identity()));
    EXPECT_THROW(
        repository.validate({std::string(temporary_forum), "other"}),
        SessionNotFoundError);
    EXPECT_THROW(
        (void)repository.create(temporary_forum, "Rejected"),
        ForumNotFoundError);

    const PreparedSession prepared = repository.prepare(temporary_identity());
    EXPECT_EQ(prepared.label, "Welcome");
    EXPECT_TRUE(prepared.lease.active());
    EXPECT_TRUE(prepared.restore.entries.empty());
}

TEST_F(SessionRepositoryTest, GivesEachRepositoryItsOwnPrivateTemporaryDatabase) {
    const SessionRepository first = make_repository();
    const SessionRepository second = make_repository();
    EXPECT_NE(
        first.prepare(temporary_identity()).database_path,
        second.prepare(temporary_identity()).database_path);
}

TEST_F(SessionRepositoryTest, RemovesOnlyItsOwnedTemporaryDirectoryOnDestruction) {
    std::filesystem::path database_path;
    {
        const SessionRepository repository = make_repository();
        database_path = repository.prepare(temporary_identity()).database_path;
        ASSERT_TRUE(std::filesystem::exists(database_path));
    }
    EXPECT_FALSE(std::filesystem::exists(database_path));
    EXPECT_FALSE(std::filesystem::exists(database_path.parent_path()));
    EXPECT_TRUE(std::filesystem::exists(fixture_.root()));
}

TEST_F(SessionRepositoryTest, HoldsOneLeasePerSessionAndReleasesItWithThePreparedValue) {
    const SessionRepository repository = make_repository();
    const StoredSession created = repository.create("lobby", "Leased");
    {
        const PreparedSession held = repository.prepare(created.identity);
        EXPECT_TRUE(held.lease.active());
        EXPECT_THROW((void)repository.prepare(created.identity), SessionBusyError);
    }
    EXPECT_NO_THROW((void)repository.prepare(created.identity));
}

TEST_F(SessionRepositoryTest, RejectsMismatchedMetadataAfterTheLeaseAndReleasesIt) {
    const SessionRepository repository = make_repository();
    const StoredSession created = repository.create("lobby", "Renamed later");
    const std::filesystem::path renamed =
        sessions_directory() / "renamed.sqlite3";
    std::filesystem::rename(created.database_path, renamed);
    const SessionIdentity wrong_filename{"lobby", "renamed"};

    EXPECT_THROW((void)repository.prepare(wrong_filename), std::runtime_error);
    // The failing read happened behind the lease, so the lease must be gone
    // again once it threw.
    EXPECT_NO_THROW((void)SessionLease::acquire(renamed));

    // Same for a database that names another forum.
    const std::filesystem::path foreign =
        sessions_directory() / "foreign.sqlite3";
    ASSERT_TRUE(create_session_database(
        foreign, {.id = "foreign", .forum = "hall", .label = "Elsewhere"}));

    EXPECT_THROW(
        (void)repository.prepare({"lobby", "foreign"}), std::runtime_error);
    EXPECT_NO_THROW((void)SessionLease::acquire(foreign));
}

TEST_F(SessionRepositoryTest, LeavesNoCompanionLockBehindForAMissingSession) {
    const SessionRepository repository = make_repository();
    (void)repository.create("lobby", "Present");

    EXPECT_THROW((void)repository.prepare({"lobby", "absent"}), SessionNotFoundError);
    EXPECT_FALSE(std::filesystem::exists(SessionLease::companion_path(
        sessions_directory() / "absent.sqlite3")));
}

TEST_F(SessionRepositoryTest, ReportsBusyBeforeInvalidForALeasedDamagedDatabase) {
    const SessionRepository repository = make_repository();
    const StoredSession created = repository.create("lobby", "Damaged and held");
    SessionLease held = SessionLease::acquire(created.database_path);
    {
        std::ofstream database(
            created.database_path, std::ios::binary | std::ios::trunc);
        database << "not SQLite";
    }

    // Preparation reads nothing until it owns the session, so contention is
    // reported ahead of the invalidity it would otherwise have found.
    EXPECT_THROW((void)repository.prepare(created.identity), SessionBusyError);
    // The route-level preflight is what still separates the two for callers.
    EXPECT_THROW(repository.validate(created.identity), std::runtime_error);
}

TEST_F(SessionRepositoryTest, RestoresTranscriptStateAndInterruptedTurnsInOneRead) {
    const SessionRepository repository = make_repository();
    const StoredSession created = repository.create("lobby", "Resumed");
    const TranscriptEntry answered = test::human_entry(
        1, {"human", "You"}, {"guide-id", "Guide"}, "Answered", 1);
    const TranscriptEntry response = make_character_entry(
        2, "guide-id", "Guide", "An answer", EntryStatus::complete, 1);
    const TranscriptEntry pending = test::human_entry(
        3, {"human", "You"}, {"guide-id", "Guide"}, "Interrupted", 2);
    {
        SessionJournal journal(created.database_path);
        journal.start_turn(1, answered);
        journal.complete_turn(1, response);
        journal.start_turn(2, pending);
    }

    const PreparedSession prepared = repository.prepare(created.identity);
    EXPECT_EQ(prepared.label, "Resumed");
    EXPECT_EQ(
        prepared.restore.entries,
        (std::vector<TranscriptEntry>{answered, response, pending}));
    EXPECT_EQ(prepared.restore.next_request_id, 3U);
    // The repair entry for the interrupted turn takes the next entry ID.
    EXPECT_EQ(prepared.restore.next_entry_id, 5U);
    ASSERT_EQ(prepared.restore.interrupted_turns.size(), 1U);
    EXPECT_EQ(prepared.restore.interrupted_turns.front().request_id, 2U);
    EXPECT_EQ(
        prepared.restore.interrupted_turns.front().error_entry.kind,
        EntryKind::error);
}

TEST_F(SessionRepositoryTest, RejectsAnUnsupportedDatabaseWhereverItIsRead) {
    const SessionRepository repository = make_repository();
    const std::filesystem::path path =
        sessions_directory() / "unsupported.sqlite3";
    std::filesystem::create_directories(sessions_directory());
    {
        std::ofstream database(path, std::ios::binary);
        database << "unsupported database";
    }
    const SessionIdentity identity{"lobby", "unsupported"};

    EXPECT_THROW(repository.validate(identity), std::runtime_error);
    EXPECT_THROW((void)repository.prepare(identity), std::runtime_error);
    EXPECT_NO_THROW((void)SessionLease::acquire(path));
}

TEST_F(SessionRepositoryTest, ServesConcurrentReadsAndCreationsWithoutARepositoryLock) {
    const SessionRepository repository = make_repository();
    (void)repository.create("lobby", "Existing");

    std::vector<std::thread> workers;
    std::atomic<int> failures{0};
    for (int worker = 0; worker != 4; ++worker) {
        workers.emplace_back([&repository, &failures, worker] {
            try {
                for (int round = 0; round != 5; ++round) {
                    (void)repository.list("lobby");
                    (void)repository.list(temporary_forum);
                    (void)repository.create("lobby", "Worker " + std::to_string(worker));
                }
            } catch (...) {
                ++failures;
            }
        });
    }
    for (std::thread& worker : workers) worker.join();

    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(repository.list("lobby").size(), 21U);
}

} // namespace
} // namespace cha
