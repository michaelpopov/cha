#include "session/session_repository.h"

#include "session/not_found_error.h"
#include "session/session_lease.h"
#include "support/test_workspace.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <map>
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

    const SessionEntry created = repository.create("lobby", "Stored");
    EXPECT_EQ(created.identity.forum_id, "lobby");
    EXPECT_EQ(created.label, "Stored");
    EXPECT_TRUE(created.error.empty());

    const std::vector<SessionEntry> listed = repository.list("lobby");
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
    const SessionEntry first = repository.create("lobby", "");
    const SessionEntry second = repository.create("lobby", "Second");
    EXPECT_EQ(first.label, first.identity.session_id);

    std::vector<std::string> ids;
    for (const SessionEntry& entry : repository.list("lobby")) {
        ids.push_back(entry.identity.session_id);
    }
    ASSERT_EQ(ids.size(), 2U);
    std::vector<std::string> expected{first.identity.session_id, second.identity.session_id};
    std::sort(expected.begin(), expected.end());
    EXPECT_EQ(ids, expected);
}

TEST_F(SessionRepositoryTest, ListsAnEmptyForumWithoutCreatingItsDirectory) {
    const SessionRepository repository = make_repository();
    EXPECT_TRUE(repository.list("lobby").empty());
    EXPECT_FALSE(std::filesystem::exists(sessions_directory()));
}

TEST_F(SessionRepositoryTest, KeepsADamagedDatabaseListedButRejectsItStrictly) {
    const SessionRepository repository = make_repository();
    const SessionEntry created = repository.create("lobby", "Broken later");
    {
        std::ofstream database(
            sessions_directory() / (created.identity.session_id + ".sqlite3"),
            std::ios::binary | std::ios::trunc);
        database << "not SQLite";
    }

    const std::vector<SessionEntry> listed = repository.list("lobby");
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

    const SessionEntry created = repository.create("lobby", "Unopened");
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

    const std::vector<SessionEntry> listed = repository.list(temporary_forum);
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
    const SessionEntry created = repository.create("lobby", "Leased");
    {
        const PreparedSession held = repository.prepare(created.identity);
        EXPECT_TRUE(held.lease.active());
        EXPECT_THROW((void)repository.prepare(created.identity), SessionBusyError);
    }
    EXPECT_NO_THROW((void)repository.prepare(created.identity));
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
