#include "session/session_repository.h"

#include "session/not_found_error.h"
#include "session/session_database.h"
#include "session/sqlite_storage.h"
#include "session/workspace_session_database.h"
#include "support/test_transcript.h"
#include "support/test_workspace.h"
#include "util/private_filesystem.h"
#include "workspace/workspace.h"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <future>
#include <iomanip>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace cha {
namespace {

constexpr std::string_view temporary_forum = "temporary-forum";
constexpr std::string_view temporary_session = "temporary-session";

std::string timestamp_session_id(std::time_t value) {
    const std::tm* const local = std::localtime(&value);
    if (local == nullptr) throw std::runtime_error("Failed to read test time");
    std::ostringstream result;
    result << std::put_time(local, "%Y-%m-%d-%H-%M-%S") << "-session";
    return result.str();
}

std::vector<std::string> near_future_session_ids() {
    const std::time_t now = std::time(nullptr);
    std::vector<std::string> result;
    for (int offset = -1; offset <= 15; ++offset) {
        result.push_back(timestamp_session_id(now + offset));
    }
    return result;
}

void seed_session_id_collisions(
    const std::filesystem::path& path,
    const std::vector<std::string>& base_ids,
    std::size_t suffix_count) {
    storage::SqliteDatabase database(
        path, storage::SqliteDatabase::Mode::read_write);
    std::int64_t forum_key{};
    {
        storage::SqliteStatement forum = database.prepare(
            "SELECT forum_key FROM forums WHERE forum_id = 'lobby'");
        if (!forum.step()) throw std::runtime_error("Test forum is missing");
        forum_key = forum.integer(0);
    }
    storage::SqliteTransaction transaction(database);
    for (const std::string& base_id : base_ids) {
        for (std::size_t suffix = 1; suffix <= suffix_count; ++suffix) {
            const std::string id = suffix == 1
                ? base_id : base_id + "-" + std::to_string(suffix);
            storage::SqliteStatement insert = database.prepare(
                "INSERT INTO sessions (forum_key, session_id, label, "
                "updated_at, history_epoch, next_entry_id, next_request_id) "
                "VALUES (?1, ?2, 'Existing', 0, 1, 1, 1)",
                forum_key,
                std::string_view(id));
            insert.run();
        }
    }
    transaction.commit();
}

class SessionRepositoryTest : public testing::Test {
protected:
    void SetUp() override {
        welcome_ = fixture_.root() / "welcome";
        create_private_directory(welcome_);
        initialize_workspace_session_database_runtime(database_path());
        loadws(fixture_.root());
    }

    std::filesystem::path database_path() const {
        return fixture_.root() / "workspace.sqlite3";
    }

    SessionRepository make_repository() const {
        loadws(fixture_.root());
        return SessionRepository(
            database_path(),
            fixture_.root(),
            welcome_,
            {{std::string(temporary_forum), std::string(temporary_session)},
             "Welcome"});
    }

    static FullSessionId temporary_identity() {
        return {std::string(temporary_forum), std::string(temporary_session)};
    }

    static std::int64_t scalar(
        const std::filesystem::path& path,
        const char* sql) {
        sqlite3* database = nullptr;
        if (sqlite3_open_v2(
                path.string().c_str(), &database,
                SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
            throw std::runtime_error("Failed to open test database");
        }
        sqlite3_stmt* statement = nullptr;
        if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr)
                != SQLITE_OK
            || sqlite3_step(statement) != SQLITE_ROW) {
            sqlite3_finalize(statement);
            sqlite3_close(database);
            throw std::runtime_error("Failed to query test database");
        }
        const std::int64_t result = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
        sqlite3_close(database);
        return result;
    }

    test::TestWorkspace fixture_;
    std::filesystem::path welcome_;
};

TEST_F(SessionRepositoryTest, CreatesOneWorkspaceDatabaseAndPreparesByKey) {
    const SessionRepository repository = make_repository();
    const StoredSession created = repository.create("lobby", "Stored");

    EXPECT_TRUE(std::filesystem::is_regular_file(database_path()));
    EXPECT_FALSE(std::filesystem::exists(
        fixture_.root() / "workspace.sqlite3.cha-lock"));
    EXPECT_FALSE(std::filesystem::exists(
        fixture_.root() / "sessions.sqlite3"));
    EXPECT_FALSE(std::filesystem::exists(
        fixture_.root() / "sessions.sqlite3.cha-lock"));
    EXPECT_FALSE(std::filesystem::exists(
        fixture_.root() / "forums" / "lobby" / "sessions"));
    EXPECT_EQ(repository.list("lobby"),
        (std::vector<StoredSession>{created}));

    const PreparedSession prepared = repository.prepare(created.identity);
    EXPECT_EQ(prepared.database_path, database_path());
    EXPECT_GT(prepared.session_key, 0);
    EXPECT_EQ(prepared.label, "Stored");
    EXPECT_TRUE(prepared.restore.entries.empty());
}

TEST_F(SessionRepositoryTest, CreatesInNewlyPublishedUnsynchronizedForum) {
    const SessionRepository repository = make_repository();
    fixture_.add_forum("second", "Second", "guide");
    loadws(fixture_.root());
    ASSERT_EQ(scalar(repository.database_path(),
        "SELECT COUNT(*) FROM forums WHERE forum_id = 'second'"), 0);

    const StoredSession created = repository.create("second", "Stored");

    EXPECT_EQ(created.identity.forum_id, "second");
    EXPECT_EQ(repository.list("second"), (std::vector<StoredSession>{created}));
    EXPECT_EQ(scalar(repository.database_path(),
        "SELECT COUNT(*) FROM forums WHERE forum_id = 'second'"), 1);
}

TEST_F(SessionRepositoryTest, CreatesRenamesAndArchivesRowsTransactionally) {
    const SessionRepository repository = make_repository();
    const StoredSession created = repository.create("lobby", "Before");
    const PreparedSession prepared = repository.prepare(created.identity);
    {
        SessionJournal journal(prepared.database_path, prepared.session_key);
        journal.record_entry(test::human_entry(
            1, {"human", "You"}, {"guide", "Guide"}, "Retained"));
    }
    const StoredSession renamed = repository.rename(created.identity, "After ✓");
    EXPECT_EQ(renamed.identity, created.identity);
    EXPECT_EQ(repository.prepare(created.identity).label, "After ✓");

    repository.archive(created.identity);
    EXPECT_TRUE(repository.list("lobby").empty());
    EXPECT_THROW(
        (void)repository.prepare(created.identity), SessionNotFoundError);
    EXPECT_EQ(scalar(repository.database_path(),
        "SELECT COUNT(*) FROM sessions WHERE archived_at IS NOT NULL"), 1);
    EXPECT_EQ(scalar(repository.database_path(), "SELECT COUNT(*) FROM entries"), 1);
}

TEST_F(SessionRepositoryTest, AppliesLabelPolicyAndSeparatesMissingForums) {
    const SessionRepository repository = make_repository();
    EXPECT_THROW(
        (void)repository.create("lobby", " leading"), std::invalid_argument);
    EXPECT_THROW(
        (void)repository.create("absent", "Label"), ForumNotFoundError);
    EXPECT_THROW(
        (void)repository.prepare({"absent", "session"}), ForumNotFoundError);
    EXPECT_THROW(
        (void)repository.prepare({"lobby", "absent"}), SessionNotFoundError);

    const StoredSession created = repository.create("lobby", "");
    EXPECT_EQ(created.label, created.identity.session_id);
    EXPECT_THROW(
        (void)repository.rename(created.identity, ""), std::invalid_argument);
}

TEST_F(SessionRepositoryTest, RetriesAnExpectedSessionIdCollision) {
    const SessionRepository repository = make_repository();
    const std::vector<std::string> base_ids = near_future_session_ids();
    seed_session_id_collisions(repository.database_path(), base_ids, 1);

    const StoredSession created = repository.create("lobby", "Created");
    EXPECT_TRUE(std::any_of(
        base_ids.begin(), base_ids.end(),
        [&created](const std::string& base_id) {
            return created.identity.session_id == base_id + "-2";
        }));
}

TEST_F(SessionRepositoryTest, BoundsSessionIdCollisionRetries) {
    const SessionRepository repository = make_repository();
    seed_session_id_collisions(
        repository.database_path(), near_future_session_ids(), 100);

    try {
        (void)repository.create("lobby", "Created");
        FAIL() << "Expected session ID allocation to fail";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(
            std::string(error.what()).find("after 100 attempts"),
            std::string::npos);
    }
}

TEST_F(SessionRepositoryTest, SurfacesUnexpectedInsertConstraints) {
    const SessionRepository repository = make_repository();
    {
        storage::SqliteDatabase database(
            repository.database_path(),
            storage::SqliteDatabase::Mode::read_write);
        database.execute(R"sql(
            CREATE TRIGGER reject_new_session
            BEFORE INSERT ON sessions
            BEGIN
                SELECT RAISE(ABORT, 'future session constraint');
            END
        )sql");
    }

    try {
        (void)repository.create("lobby", "Created");
        FAIL() << "Expected the constraint failure to escape";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(
            std::string(error.what()).find("future session constraint"),
            std::string::npos);
    }
}

TEST_F(SessionRepositoryTest, DoesNotRetryAnUnrelatedUniqueConstraint) {
    const SessionRepository repository = make_repository();
    (void)repository.create("lobby", "Existing");
    {
        storage::SqliteDatabase database(
            repository.database_path(),
            storage::SqliteDatabase::Mode::read_write);
        database.execute(
            "CREATE UNIQUE INDEX one_session_total ON sessions ((1))");
    }

    try {
        (void)repository.create("lobby", "Created");
        FAIL() << "Expected the unrelated unique constraint to escape";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(
            std::string(error.what()).find("one_session_total"),
            std::string::npos);
    }
}

TEST_F(SessionRepositoryTest, KeepsTwoSessionsInOneDatabaseIsolated) {
    const SessionRepository repository = make_repository();
    const PreparedSession first =
        repository.prepare(repository.create("lobby", "First").identity);
    const PreparedSession second =
        repository.prepare(repository.create("lobby", "Second").identity);
    ASSERT_NE(first.session_key, second.session_key);
    ASSERT_EQ(first.database_path, second.database_path);

    const TranscriptEntry first_prompt = test::human_entry(
        1, {"human", "You"}, {"guide", "Guide"}, "First prompt", 1);
    const TranscriptEntry second_prompt = test::human_entry(
        1, {"human", "You"}, {"guide", "Guide"}, "Second prompt", 1);
    {
        SessionJournal first_journal(first.database_path, first.session_key);
        SessionJournal second_journal(second.database_path, second.session_key);
        first_journal.start_turn(1, first_prompt);
        second_journal.start_turn(1, second_prompt);
        first_journal.cancel_turn(1, std::nullopt);
        second_journal.cancel_turn(1, std::nullopt);
    }

    EXPECT_EQ(repository.prepare(first.identity).restore.entries,
        (std::vector<TranscriptEntry>{first_prompt}));
    EXPECT_EQ(repository.prepare(second.identity).restore.entries,
        (std::vector<TranscriptEntry>{second_prompt}));

    {
        SessionJournal first_journal(first.database_path, first.session_key);
        first_journal.clear();
    }
    EXPECT_TRUE(repository.prepare(first.identity).restore.entries.empty());
    EXPECT_EQ(repository.prepare(second.identity).restore.entries,
        (std::vector<TranscriptEntry>{second_prompt}));
}

TEST_F(SessionRepositoryTest, ReadsHistoryWhileAnotherSessionHasAStartedTurn) {
    const SessionRepository repository = make_repository();
    const PreparedSession first =
        repository.prepare(repository.create("lobby", "First").identity);
    const PreparedSession second =
        repository.prepare(repository.create("lobby", "Second").identity);
    const TranscriptEntry prompt = test::human_entry(
        1, {"human", "You"}, {"guide", "Guide"}, "Pending", 1);
    SessionJournal journal(first.database_path, first.session_key);
    journal.start_turn(1, prompt);

    EXPECT_TRUE(repository.history(second.identity).empty());
    EXPECT_EQ(repository.history(first.identity),
        (std::vector<TranscriptEntry>{prompt}));
}

TEST_F(SessionRepositoryTest, WelcomeUsesTheSameSchemaInAPrivateFile) {
    std::filesystem::path welcome_path;
    {
        const SessionRepository repository = make_repository();
        const PreparedSession welcome = repository.prepare(temporary_identity());
        welcome_path = welcome.database_path;
        EXPECT_NE(welcome.database_path, repository.database_path());
        EXPECT_EQ(welcome.session_key, 1);
        EXPECT_EQ(repository.list(temporary_forum).size(), 1U);
        EXPECT_EQ(scalar(welcome.database_path, "PRAGMA application_id"),
            0x43484157);
        EXPECT_EQ(scalar(welcome.database_path, "SELECT COUNT(*) FROM forums"),
            1);
        EXPECT_EQ(scalar(welcome.database_path, "SELECT COUNT(*) FROM sessions"),
            1);
        EXPECT_THROW(
            (void)repository.create(temporary_forum, "No"),
            ForumNotFoundError);
#ifndef _WIN32
        struct stat info {};
        ASSERT_EQ(::lstat(welcome.database_path.parent_path().c_str(), &info), 0);
        EXPECT_EQ(info.st_mode & 0777, static_cast<mode_t>(0700));
#endif
    }
    EXPECT_TRUE(std::filesystem::exists(welcome_path.parent_path()));
    EXPECT_TRUE(std::filesystem::exists(welcome_path));
}

TEST_F(SessionRepositoryTest, SynchronizesForumsWithoutDeletingOldRows) {
    fixture_.add_forum("second", "Second", "guide");
    const SessionRepository repository = make_repository();
    const StoredSession stored = repository.create("second", "Kept");

    const std::filesystem::path forum = fixture_.root() / "forums" / "second";
    const std::filesystem::path aside = fixture_.root() / "second-aside";
    std::filesystem::rename(forum, aside);
    Workspace without = Workspace::load(fixture_.root());
    repository.synchronize_forums(without);
    loadws(std::move(without));
    EXPECT_THROW((void)repository.list("second"), ForumNotFoundError);
    EXPECT_TRUE(std::ranges::none_of(
        repository.recent(),
        [&stored](const StoredSession& recent) {
            return recent.identity == stored.identity;
        }));

    std::filesystem::rename(aside, forum);
    Workspace restored = Workspace::load(fixture_.root());
    repository.synchronize_forums(restored);
    loadws(std::move(restored));
    ASSERT_EQ(repository.list("second").size(), 1U);
    EXPECT_EQ(repository.list("second").front().identity, stored.identity);
    EXPECT_TRUE(std::ranges::any_of(
        repository.recent(),
        [&stored](const StoredSession& recent) {
            return recent.identity == stored.identity;
        }));
}

TEST_F(SessionRepositoryTest, ConcurrentCreatesUseShortLivedConnections) {
    const SessionRepository repository = make_repository();
    std::atomic<int> failures{};
    std::vector<std::thread> workers;
    for (int index = 0; index != 8; ++index) {
        workers.emplace_back([&repository, &failures, index] {
            try {
                (void)repository.create(
                    "lobby", "Worker " + std::to_string(index));
            } catch (...) {
                ++failures;
            }
        });
    }
    for (std::thread& worker : workers) worker.join();
    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(repository.list("lobby").size(), 8U);
}

TEST_F(SessionRepositoryTest, MaintenanceFencesRepositoryReadsAndWrites) {
    using namespace std::chrono_literals;
    const SessionRepository repository = make_repository();
    const StoredSession rename_target =
        repository.create("lobby", "Rename target");
    const StoredSession archive_target =
        repository.create("lobby", "Archive target");
    std::future<StoredSession> create;
    std::future<StoredSession> rename;
    std::future<void> archive;
    std::future<std::vector<StoredSession>> list;
    {
        const SessionRepository::MaintenanceGuard maintenance =
            repository.reserve_maintenance();
        create = std::async(std::launch::async, [&] {
            return repository.create("lobby", "Created after maintenance");
        });
        rename = std::async(std::launch::async, [&] {
            return repository.rename(rename_target.identity, "Renamed");
        });
        archive = std::async(std::launch::async, [&] {
            repository.archive(archive_target.identity);
        });
        list = std::async(std::launch::async, [&] {
            return repository.list("lobby");
        });

        EXPECT_EQ(create.wait_for(100ms), std::future_status::timeout);
        EXPECT_EQ(rename.wait_for(0ms), std::future_status::timeout);
        EXPECT_EQ(archive.wait_for(0ms), std::future_status::timeout);
        EXPECT_EQ(list.wait_for(0ms), std::future_status::timeout);
        EXPECT_NO_THROW(maintenance.checkpoint());
    }

    EXPECT_EQ(create.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(rename.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(archive.wait_for(2s), std::future_status::ready);
    EXPECT_EQ(list.wait_for(2s), std::future_status::ready);
    EXPECT_NO_THROW((void)create.get());
    EXPECT_NO_THROW((void)rename.get());
    EXPECT_NO_THROW(archive.get());
    EXPECT_NO_THROW((void)list.get());
}

TEST_F(SessionRepositoryTest, CompetingWriterWaitsAtImmediateTransactionStart) {
    using namespace std::chrono_literals;
    const SessionRepository repository = make_repository();
    const PreparedSession first =
        repository.prepare(repository.create("lobby", "First").identity);
    const PreparedSession second =
        repository.prepare(repository.create("lobby", "Second").identity);
    SessionJournal second_journal(second.database_path, second.session_key);
    const TranscriptEntry prompt = test::human_entry(
        1, {"human", "You"}, {"guide", "Guide"}, "Waiting", 1);

    storage::SqliteDatabase writer(
        repository.database_path(), storage::SqliteDatabase::Mode::read_write);
    storage::SqliteTransaction transaction(writer);
    storage::SqliteStatement update = writer.prepare(
        "UPDATE sessions SET updated_at = updated_at + 1 "
        "WHERE session_key = ?1",
        first.session_key);
    update.run();

    auto competing = std::async(std::launch::async, [&] {
        second_journal.start_turn(1, prompt);
    });
    EXPECT_EQ(competing.wait_for(100ms), std::future_status::timeout);
    transaction.commit();
    EXPECT_EQ(competing.wait_for(2s), std::future_status::ready);
    EXPECT_NO_THROW(competing.get());
    EXPECT_EQ(repository.history(second.identity),
        (std::vector<TranscriptEntry>{prompt}));
}

} // namespace
} // namespace cha
